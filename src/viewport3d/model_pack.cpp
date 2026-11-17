/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file model_pack.cpp Loading the package of 3D models that replace sprites in the 3D viewport. */

#include "../stdafx.h"
#include "model_pack.h"
#include "../debug.h"
#include "../engine_base.h"
#include "../fileio_func.h"
#include "../newgrf_commons.h"
#include "../newgrf_type.h"
#include "../palette_func.h"
#include "../gfx_func.h"
#include "../3rdparty/nlohmann/json.hpp"

#include "../safeguards.h"

/*
 * The package is a folder "opengfx3d" in any of the places the game looks for its data.
 * Its index, "opengfx3d.json", lists the models and what they replace:
 *
 * {
 *   "name": "OpenGFX3D",
 *   "vehicles": [
 *     { "type": "road", "grfid": "00000000", "ids": [0, 1, 2], "model": "bus.glb", "scale": 1.0 }
 *   ]
 * }
 *
 * "type" is "train", "road", "ship" or "aircraft". "grfid" is the ID of the NewGRF that defines the vehicles as
 * eight hexadecimal digits, the way the game shows it; leave it out for the vehicles of the base game.
 * "ids" are the numbers of the vehicles within that NewGRF or within the base game. "scale" is optional.
 */

static const std::string_view PACK_DIRECTORY = "opengfx3d/";
static const std::string_view PACK_INDEX = "opengfx3d.json";

/**
 * Read a file of the package.
 * @param filename Name within the folder of the package.
 * @return The contents, or nothing when the file is missing or unreasonably large.
 */
static std::optional<std::vector<uint8_t>> ReadPackFile(std::string_view filename)
{
	static constexpr size_t MAX_SIZE = 64 << 20;

	size_t size;
	auto file = FioFOpenFile(fmt::format("{}{}", PACK_DIRECTORY, filename), "rb", Subdirectory::Base, &size);
	if (!file.has_value() || size > MAX_SIZE) return std::nullopt;

	std::vector<uint8_t> data(size);
	if (size != 0 && fread(data.data(), 1, size, *file) != size) return std::nullopt;
	return data;
}

/**
 * Load a model of the package, once.
 * @param filename Its file name.
 * @param loaded Models that were already read, by file name.
 * @return Its index in #models, or \c UINT_MAX when it cannot be used.
 */
uint ModelPack::LoadModel(const std::string &filename, std::map<std::string, uint> &loaded)
{
	/* File names stay within the package. */
	if (filename.empty() || filename.find_first_of("/\\:") != std::string::npos || filename.starts_with(".")) {
		Debug(Facility::Misc, Severity::Error, "[viewport3d] {}: bad file name '{}'", PACK_INDEX, filename);
		return UINT_MAX;
	}

	auto [it, inserted] = loaded.try_emplace(filename, UINT_MAX);
	if (inserted) {
		auto data = ReadPackFile(filename);
		Model3D model;
		std::string error = "file not found";
		if (data.has_value() && LoadGlbModel(*data, model, error)) {
			it->second = static_cast<uint>(this->models.size());
			this->models.push_back(std::move(model));
		} else {
			Debug(Facility::Misc, Severity::Error, "[viewport3d] cannot use model '{}': {}", filename, error);
		}
	}
	return it->second;
}

/**
 * Read the entries of one list of the index.
 * @param list The list.
 * @param kinds The words that the entries may use for "type", and what they mean.
 * @param loaded Models that were already read, by file name.
 */
void ModelPack::ReadEntries(const nlohmann::json &list, std::initializer_list<std::pair<std::string_view, ModelKind>> kinds, std::map<std::string, uint> &loaded)
{
	for (const auto &entry : list) {
		std::string type_name = entry.at("type").get<std::string>();
		auto kind = std::ranges::find(kinds, type_name, &std::pair<std::string_view, ModelKind>::first);
		if (kind == kinds.end()) {
			Debug(Facility::Misc, Severity::Error, "[viewport3d] {}: unknown type '{}'", PACK_INDEX, type_name);
			continue;
		}

		uint model = this->LoadModel(entry.at("model").get<std::string>(), loaded);
		if (model == UINT_MAX) continue;

		/* Helicopters have a main rotor that turns. */
		uint rotor = UINT_MAX;
		std::array<float, 3> rotor_at{};
		if (entry.contains("rotor")) {
			rotor = this->LoadModel(entry.at("rotor").get<std::string>(), loaded);
			if (entry.contains("rotor_at")) rotor_at = entry.at("rotor_at").get<std::array<float, 3>>();
		}

		/* A simpler version to draw far from the camera; an empty name leaves the model out there. */
		if (entry.contains("far")) {
			std::string far_name = entry.at("far").get<std::string>();
			uint far = far_name.empty() ? FAR_HIDDEN : this->LoadModel(far_name, loaded);
			if (far != UINT_MAX || far_name.empty()) {
				if (this->far.size() <= model) this->far.resize(model + 1, FAR_SAME);
				this->far[model] = far;
			}
		}

		/* The version for the sub-arctic climate, such as a forest with snow on its trees. */
		uint snow = entry.contains("snow") ? this->LoadModel(entry.at("snow").get<std::string>(), loaded) : UINT_MAX;

		/* Parts that move. */
		std::vector<ModelPart> parts;
		if (entry.contains("parts")) {
			for (const auto &item : entry.at("parts")) {
				ModelPart part;
				part.model = this->LoadModel(item.at("model").get<std::string>(), loaded);
				if (part.model == UINT_MAX) continue;
				if (item.contains("at")) part.at = item.at("at").get<std::array<float, 3>>();
				if (item.contains("axis")) part.axis = item.at("axis").get<std::array<float, 3>>();
				part.spin = item.value("spin", 0.0f);
				part.swing = item.value("swing", 0.0f);
				part.period = std::max(0.05f, item.value("period", 1.0f));
				part.lift = item.value("lift", 0.0f);
				parts.push_back(part);
			}
		}

		uint32_t grfid = 0;
		std::string grfid_text = entry.value("grfid", std::string{"0"});
		auto [end, result] = std::from_chars(grfid_text.data(), grfid_text.data() + grfid_text.size(), grfid, 16);
		if (result != std::errc{} || end != grfid_text.data() + grfid_text.size()) {
			Debug(Facility::Misc, Severity::Error, "[viewport3d] {}: bad NewGRF ID '{}'", PACK_INDEX, grfid_text);
			continue;
		}

		float scale = entry.value("scale", 1.0f);
		for (const auto &id : entry.at("ids")) {
			this->entries[{kind->second, grfid, id.get<uint16_t>()}] = {model, scale, rotor, rotor_at, parts, snow};
		}
	}
}

/** Find the package and load it. Without a package, or with a broken one, nothing has a model. */
ModelPack::ModelPack()
{
	auto index = ReadPackFile(PACK_INDEX);
	if (!index.has_value()) return;

	try {
		nlohmann::json json = nlohmann::json::parse(index->begin(), index->end());
		std::map<std::string, uint> loaded;

		if (json.contains("vehicles")) {
			this->ReadEntries(json.at("vehicles"), {
				{"train", ModelKind::Train}, {"road", ModelKind::Road}, {"ship", ModelKind::Ship}, {"aircraft", ModelKind::Aircraft},
			}, loaded);
		}
		if (json.contains("buildings")) {
			this->ReadEntries(json.at("buildings"), {
				{"house", ModelKind::House}, {"industry_tile", ModelKind::IndustryTile}, {"rail_station", ModelKind::RailStation},
				{"road_stop", ModelKind::RoadStop}, {"object", ModelKind::Object}, {"airport_tile", ModelKind::AirportTile},
				{"dock", ModelKind::Dock}, {"buoy", ModelKind::Buoy}, {"rail_depot", ModelKind::RailDepot},
				{"road_depot", ModelKind::RoadDepot}, {"ship_depot", ModelKind::ShipDepot},
				{"tree", ModelKind::Tree}, {"field", ModelKind::Field}, {"rough", ModelKind::Rough}, {"rocks", ModelKind::Rocks}, {"effect", ModelKind::Effect}, {"track", ModelKind::Track}, {"signal", ModelKind::Signal},
			}, loaded);
		}

		this->name = json.value("name", std::string{"OpenGFX3D"});
		Debug(Facility::Driver, Severity::Error, "[viewport3d] model package '{}': {} models for {} vehicle and building types", this->name, this->models.size(), this->entries.size());
	} catch (const std::exception &e) {
		Debug(Facility::Misc, Severity::Error, "[viewport3d] cannot read {}: {}", PACK_INDEX, e.what());
		this->models.clear();
		this->entries.clear();
	}
}

/**
 * Find the model for something.
 * @param kind What it is.
 * @param grfid The NewGRF it is from as a number, 0 for the base game.
 * @param id Its number within the NewGRF or the base game.
 * @return The model, or \c nullptr when the package has none for it.
 */
const ModelPack::Entry *ModelPack::Find(ModelKind kind, uint32_t grfid, uint16_t id) const
{
	auto it = this->entries.find({kind, grfid, id});
	return it == this->entries.end() ? nullptr : &it->second;
}

/**
 * Find the model for something that a NewGRF may have defined.
 * @param kind What it is.
 * @param props Where it comes from.
 * @param base_game_id Its number in the base game, used when no NewGRF defines it.
 * @return The model, or \c nullptr when the package has none for it.
 */
const ModelPack::Entry *ModelPack::Find(ModelKind kind, const GRFFilePropsBase &props, uint16_t base_game_id) const
{
	if (props.HasGrfFile()) return this->Find(kind, GetGrfIDNumber(props.grfid), props.local_id);
	return this->Find(kind, 0, base_game_id);
}

/**
 * Find the model for vehicles with an engine.
 * @param e The engine.
 * @return The model, or \c nullptr when the package has none for it.
 */
const ModelPack::VehicleModel *ModelPack::FindVehicle(const Engine *e) const
{
	if (this->entries.empty() || e == nullptr) return nullptr;
	ModelKind kind;
	switch (e->type) {
		case VehicleType::Train: kind = ModelKind::Train; break;
		case VehicleType::Road: kind = ModelKind::Road; break;
		case VehicleType::Ship: kind = ModelKind::Ship; break;
		case VehicleType::Aircraft: kind = ModelKind::Aircraft; break;
		default: return nullptr;
	}
	return this->Find(kind, GetGrfIDNumber(e->GetGRFID()), e->grf_prop.local_id);
}

/**
 * Get a NewGRF ID as the number that the game shows for it, and that the index uses.
 * @param grfid The ID.
 * @return The number.
 */
uint32_t ModelPack::GetGrfIDNumber(GrfID grfid)
{
	return static_cast<uint32_t>(grfid[0]) << 24 | grfid[1] << 16 | grfid[2] << 8 | grfid[3];
}

/**
 * Get the colour that parts of models named "company" are multiplied with.
 * @param colour One of the colours of companies, which houses and industries use too.
 * @return The colour, each channel from 0 to 1.
 */
std::array<float, 4> ModelPack::GetTint(Colours colour)
{
	const Colour &c = _cur_palette.palette[GetColourGradient(colour, Shade::Normal).p];
	return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1};
}

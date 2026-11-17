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
#include "../newgrf_type.h"
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

/** Find the package and load it. Without a package, or with a broken one, no vehicle has a model. */
ModelPack::ModelPack()
{
	auto index = ReadPackFile(PACK_INDEX);
	if (!index.has_value()) return;

	try {
		nlohmann::json json = nlohmann::json::parse(index->begin(), index->end());
		std::map<std::string, uint> loaded; // Models by file name.

		for (const auto &entry : json.at("vehicles")) {
			VehicleType type;
			std::string type_name = entry.at("type").get<std::string>();
			if (type_name == "train") {
				type = VehicleType::Train;
			} else if (type_name == "road") {
				type = VehicleType::Road;
			} else if (type_name == "ship") {
				type = VehicleType::Ship;
			} else if (type_name == "aircraft") {
				type = VehicleType::Aircraft;
			} else {
				Debug(Facility::Misc, Severity::Error, "[viewport3d] {}: unknown kind of vehicle '{}'", PACK_INDEX, type_name);
				continue;
			}

			/* File names stay within the package. */
			std::string filename = entry.at("model").get<std::string>();
			if (filename.empty() || filename.find_first_of("/\\:") != std::string::npos || filename.starts_with(".")) {
				Debug(Facility::Misc, Severity::Error, "[viewport3d] {}: bad file name '{}'", PACK_INDEX, filename);
				continue;
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
			if (it->second == UINT_MAX) continue;

			uint32_t grfid = 0;
			std::string grfid_text = entry.value("grfid", std::string{"0"});
			auto [end, result] = std::from_chars(grfid_text.data(), grfid_text.data() + grfid_text.size(), grfid, 16);
			if (result != std::errc{} || end != grfid_text.data() + grfid_text.size()) {
				Debug(Facility::Misc, Severity::Error, "[viewport3d] {}: bad NewGRF ID '{}'", PACK_INDEX, grfid_text);
				continue;
			}
			float scale = entry.value("scale", 1.0f);
			for (const auto &id : entry.at("ids")) {
				this->vehicles[{type, grfid, id.get<uint16_t>()}] = {it->second, scale};
			}
		}

		this->name = json.value("name", std::string{"OpenGFX3D"});
		Debug(Facility::Driver, Severity::Error, "[viewport3d] model package '{}': {} models for {} vehicles", this->name, this->models.size(), this->vehicles.size());
	} catch (const std::exception &e) {
		Debug(Facility::Misc, Severity::Error, "[viewport3d] cannot read {}: {}", PACK_INDEX, e.what());
		this->models.clear();
		this->vehicles.clear();
	}
}

/**
 * Find the model for vehicles with an engine.
 * @param e The engine.
 * @return The model, or \c nullptr when the package has none for it.
 */
const ModelPack::VehicleModel *ModelPack::FindVehicle(const Engine *e) const
{
	if (this->vehicles.empty() || e == nullptr) return nullptr;
	GrfID id = e->GetGRFID();
	uint32_t grfid = static_cast<uint32_t>(id[0]) << 24 | id[1] << 16 | id[2] << 8 | id[3];
	auto it = this->vehicles.find({e->type, grfid, e->grf_prop.local_id});
	return it == this->vehicles.end() ? nullptr : &it->second;
}

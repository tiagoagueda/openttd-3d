/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file model_pack.h The package of 3D models that replace sprites in the 3D viewport. */

#ifndef VIEWPORT3D_MODEL_PACK_H
#define VIEWPORT3D_MODEL_PACK_H

#include "model3d.h"
#include "../3rdparty/nlohmann/json.hpp"
#include "../gfx_type.h"
#include "../newgrf_type.h"
#include "../vehicle_type.h"

class Engine;
struct GRFFilePropsBase;

/** What a model of the package replaces. */
enum class ModelKind : uint8_t {
	Train, ///< Rail vehicles, by engine.
	Road, ///< Road vehicles, by engine.
	Ship, ///< Ships, by engine.
	Aircraft, ///< Aircraft, by engine.
	House, ///< Town buildings, by house type.
	IndustryTile, ///< Tiles of industries, by industry tile type.
	RailStation, ///< Tiles of railway stations, by station type.
	RoadStop, ///< Bus and lorry stops, by stop type.
	Object, ///< Objects such as transmitters and company headquarters, by object type.
	AirportTile, ///< Tiles of airports, by airport tile type.
	Dock, ///< Docks: 0 the part on the shore, 1 the part on the water.
	Buoy, ///< Buoys.
	RailDepot, ///< Train depots.
	RoadDepot, ///< Road vehicle depots.
	ShipDepot, ///< Ship depots, both tiles together.
	Tree, ///< One tree, by tree type; sub-arctic trees above the snow line are the type + 0x40.
	Field, ///< A farm field, by its state from ploughed (0) to harvested with hay (8).
	Rough, ///< Rough land, by version; the game picks one for each tile.
	Rocks, ///< Rocky land, by version; the game picks one for each tile.
	Effect, ///< Smoke, steam and sparks, by effect vehicle type.
	Track, ///< Pieces of track: 0 and 1 railway, 2 and 3 monorail, 4 and 5 maglev; even straight, odd diagonal.
	Signal, ///< Rail signals, by type * 4 + variant * 2 + state.
};

/**
 * The OpenGFX3D package: 3D models, and what they replace.
 * Everything without a model keeps the sprites of the 2D game.
 */
class ModelPack {
public:
	/** A part of a model that moves, such as the beam of a pump jack. */
	struct ModelPart {
		uint model; ///< Index into GetModels().
		std::array<float, 3> at{}; ///< Where the part turns around, in the frame of the model it belongs to.
		std::array<float, 3> axis{0, 0, 1}; ///< Around which axis it turns, in that frame.
		float spin = 0; ///< Radians per second it keeps turning by.
		float swing = 0; ///< Radians it swings to either side, back and forth.
		float period = 1; ///< Seconds for one swing back and forth.
		float lift = 0; ///< World units it moves along its axis and back, once per period.
	};

	/** A model for something on the map. */
	struct VehicleModel {
		uint model; ///< Index into GetModels().
		float scale; ///< World units per unit of the model.
		uint rotor = UINT_MAX; ///< Index into GetModels() of the main rotor of a helicopter, or \c UINT_MAX.
		std::array<float, 3> rotor_at{}; ///< Where the hub of the rotor is, in the frame of the model.
		std::vector<ModelPart> parts; ///< Parts that move all the time.
		uint snow = UINT_MAX; ///< Index into GetModels() of the version in sub-arctic climate, where the 2D game draws it snowy, or \c UINT_MAX.
	};
	using Entry = VehicleModel;

	ModelPack();

	/** Name of the package, empty when there is none. */
	const std::string &GetName() const { return this->name; }

	/** All models of the package. */
	std::span<const Model3D> GetModels() const { return this->models; }

	static constexpr uint FAR_SAME = UINT_MAX; ///< GetFarModel(): the model looks the same from far away.
	static constexpr uint FAR_HIDDEN = UINT_MAX - 1; ///< GetFarModel(): the model is too small to draw from far away.

	/**
	 * What to draw in place of a model when it is far from the camera.
	 * @param model Index into GetModels().
	 * @return Index into GetModels() of a simpler version, #FAR_SAME or #FAR_HIDDEN.
	 */
	uint GetFarModel(uint model) const { return model < this->far.size() ? this->far[model] : FAR_SAME; }

	/** Whether the package has any model at all. */
	bool IsEmpty() const { return this->entries.empty(); }

	const Entry *Find(ModelKind kind, uint32_t grfid, uint16_t id) const;
	const Entry *Find(ModelKind kind, const GRFFilePropsBase &props, uint16_t base_game_id) const;
	const VehicleModel *FindVehicle(const Engine *e) const;

	static std::array<float, 4> GetTint(Colours colour);
	static uint32_t GetGrfIDNumber(GrfID grfid);

private:
	uint LoadModel(const std::string &filename, std::map<std::string, uint> &loaded);
	void ReadEntries(const nlohmann::json &list, std::initializer_list<std::pair<std::string_view, ModelKind>> kinds, std::map<std::string, uint> &loaded);

	using Key = std::tuple<ModelKind, uint32_t, uint16_t>; ///< What is replaced, the NewGRF it is from (0 for the base game) and its number within the NewGRF.

	std::string name; ///< Name of the package.
	std::vector<Model3D> models; ///< The models.
	std::vector<uint> far; ///< For each model, what to draw far away; see GetFarModel().
	std::map<Key, Entry> entries; ///< What has a model.
};

#endif /* VIEWPORT3D_MODEL_PACK_H */

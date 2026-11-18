/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file model3d.cpp Reading 3D models for the 3D viewport from glTF files. */

#include "../stdafx.h"
#include "model3d.h"
#include "../3rdparty/nlohmann/json.hpp"

#include <cfloat>

#include "../safeguards.h"

/*
 * What is read from a binary glTF 2.0 file (.glb):
 * - the triangles of all meshes of the default scene, with the transformations of their nodes;
 * - positions, normals (made from the triangles when missing) and indices;
 * - the base colour of the material as the colour of the triangles;
 * - the base colour texture of the material and the first set of texture coordinates; the texture is multiplied
 *   with the base colour, and always repeats.
 * A material with a name that starts with "company" takes the colour of the owner, multiplied with its base colour.
 * A material with "leaf", "leaves" or "foliage" in its name changes colour with the seasons.
 * Triangles of a material with a name that starts with "load" are the cargo of a vehicle: they are drawn
 * only when the vehicle carries cargo, and sink into the vehicle as it gets emptier.
 * Other textures (normal maps and the like), animations and skins are ignored.
 *
 * glTF has x to the left of the model, y up and z to its front. One unit is one world unit: a tile is 16 long.
 */

namespace {

/** Reads a glTF file; errors are reported by throwing a std::string. */
class GlbReader {
public:
	GlbReader(std::span<const uint8_t> data, Model3D &model, const ModelTextureFinder &find_texture) : model(model), find_texture(find_texture)
	{
		if (data.size() < 20 || this->Read32(data, 0) != 0x46546C67) throw std::string("not a binary glTF file");
		if (this->Read32(data, 4) != 2) throw std::string("only glTF version 2 is supported");

		size_t pos = 12;
		std::span<const uint8_t> json_chunk;
		while (pos + 8 <= data.size()) {
			uint32_t length = this->Read32(data, pos);
			uint32_t type = this->Read32(data, pos + 4);
			pos += 8;
			if (length > data.size() - pos) throw std::string("file is cut short");
			if (type == 0x4E4F534A) json_chunk = data.subspan(pos, length);
			if (type == 0x004E4942 && this->bin.empty()) this->bin = data.subspan(pos, length);
			pos += (length + 3) & ~3U;
		}
		if (json_chunk.empty()) throw std::string("no scene description found");

		this->json = nlohmann::json::parse(json_chunk.begin(), json_chunk.end(), nullptr, false);
		if (!this->json.is_object()) throw std::string("scene description is not valid JSON");

		static constexpr Transform IDENTITY{{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
		const nlohmann::json &scenes = this->json.at("scenes");
		const nlohmann::json &scene = scenes.at(this->json.value("scene", 0));
		for (const auto &node : scene.at("nodes")) this->AddNode(node.get<size_t>(), IDENTITY, 0);
		if (this->model.indices.empty() && this->load_indices.empty()) throw std::string("no triangles found");

		/* The load goes last, so that it can be drawn on its own. */
		this->model.num_fixed_indices = this->model.indices.size();
		float low = std::numeric_limits<float>::max(), high = std::numeric_limits<float>::lowest();
		for (uint32_t index : this->load_indices) {
			low = std::min(low, this->model.vertices[index].z);
			high = std::max(high, this->model.vertices[index].z);
		}
		if (!this->load_indices.empty()) this->model.load_depth = high - low;
		this->model.indices.insert(this->model.indices.end(), this->load_indices.begin(), this->load_indices.end());
	}

private:
	/** Rotation, scale and shear as a 3x3 matrix in row order, and a translation. */
	struct Transform {
		std::array<float, 9> m;
		std::array<float, 3> t;

		std::array<float, 3> Point(const std::array<float, 3> &p) const
		{
			return {
				this->m[0] * p[0] + this->m[1] * p[1] + this->m[2] * p[2] + this->t[0],
				this->m[3] * p[0] + this->m[4] * p[1] + this->m[5] * p[2] + this->t[1],
				this->m[6] * p[0] + this->m[7] * p[1] + this->m[8] * p[2] + this->t[2],
			};
		}

		std::array<float, 3> Direction(const std::array<float, 3> &p) const
		{
			return {
				this->m[0] * p[0] + this->m[1] * p[1] + this->m[2] * p[2],
				this->m[3] * p[0] + this->m[4] * p[1] + this->m[5] * p[2],
				this->m[6] * p[0] + this->m[7] * p[1] + this->m[8] * p[2],
			};
		}

		/** The transformation that does \a o first and then this one. */
		Transform operator*(const Transform &o) const
		{
			Transform r;
			for (int row = 0; row < 3; row++) {
				for (int col = 0; col < 3; col++) {
					r.m[row * 3 + col] = this->m[row * 3] * o.m[col] + this->m[row * 3 + 1] * o.m[3 + col] + this->m[row * 3 + 2] * o.m[6 + col];
				}
			}
			r.t = this->Point(o.t);
			return r;
		}
	};

	Model3D &model; ///< The model to fill.
	const ModelTextureFinder &find_texture; ///< Finds the textures.
	std::map<size_t, int> textures; ///< The textures that were looked up, by the number of their image in the file.
	std::vector<uint32_t> load_indices; ///< The triangles of the load, until they are added after the others.
	uint32_t windows = 0; ///< Makes the numbers of the windows of one kind of glass differ from those of the next.
	nlohmann::json json; ///< The scene description.
	std::span<const uint8_t> bin; ///< The binary data that the scene description refers to.

	static uint32_t Read32(std::span<const uint8_t> data, size_t pos)
	{
		return data[pos] | data[pos + 1] << 8 | data[pos + 2] << 16 | static_cast<uint32_t>(data[pos + 3]) << 24;
	}

	static float ReadFloat(std::span<const uint8_t> data, size_t pos)
	{
		return std::bit_cast<float>(Read32(data, pos));
	}

	/** The data of an accessor: where it starts, the distance between elements and their number. */
	struct Accessor {
		std::span<const uint8_t> data;
		size_t stride;
		size_t count;
		int component_type;
	};

	Accessor GetAccessor(size_t index, std::string_view type, size_t components)
	{
		const nlohmann::json &accessor = this->json.at("accessors").at(index);
		if (accessor.at("type").get<std::string>() != type) throw std::string("unexpected kind of data");
		if (!accessor.contains("bufferView") || accessor.contains("sparse")) throw std::string("sparse data is not supported");
		const nlohmann::json &view = this->json.at("bufferViews").at(accessor.at("bufferView").get<size_t>());
		if (view.value("buffer", 0) != 0) throw std::string("data outside of the file is not supported");

		Accessor result;
		result.component_type = accessor.at("componentType").get<int>();
		result.count = accessor.at("count").get<size_t>();
		size_t component_size;
		switch (result.component_type) {
			case 5121: component_size = 1; break; // Unsigned byte.
			case 5123: component_size = 2; break; // Unsigned short.
			case 5125: // Unsigned int.
			case 5126: component_size = 4; break; // Float.
			default: throw std::string("unsupported number format");
		}
		size_t element_size = component_size * components;
		result.stride = view.value("byteStride", static_cast<size_t>(0));
		if (result.stride == 0) result.stride = element_size;

		size_t offset = view.value("byteOffset", static_cast<size_t>(0)) + accessor.value("byteOffset", static_cast<size_t>(0));
		if (result.count == 0 || result.stride < element_size || offset > this->bin.size() ||
				(this->bin.size() - offset - element_size) / result.stride < result.count - 1 || this->bin.size() - offset < element_size) {
			throw std::string("data runs past the end of the file");
		}
		result.data = this->bin.subspan(offset);
		return result;
	}

	/**
	 * Find the image of a texture of the file.
	 * @param index Number of the texture in the file.
	 * @return The number of the texture for the vertices, or -1 when it cannot be used.
	 */
	int GetTexture(size_t index)
	{
		if (!this->find_texture) return -1;
		size_t source = this->json.at("textures").at(index).at("source").get<size_t>();
		auto [it, inserted] = this->textures.try_emplace(source, -1);
		if (!inserted) return it->second;

		const nlohmann::json &image = this->json.at("images").at(source);
		if (image.contains("uri")) {
			it->second = this->find_texture(image.at("uri").get<std::string>(), {});
		} else if (image.contains("bufferView")) {
			const nlohmann::json &view = this->json.at("bufferViews").at(image.at("bufferView").get<size_t>());
			size_t offset = view.value("byteOffset", static_cast<size_t>(0));
			size_t length = view.at("byteLength").get<size_t>();
			if (view.value("buffer", 0) != 0 || offset > this->bin.size() || length > this->bin.size() - offset) throw std::string("image runs past the end of the file");
			it->second = this->find_texture({}, this->bin.subspan(offset, length));
		}
		return it->second;
	}

	std::array<float, 3> ReadVec3(const Accessor &accessor, size_t i) const
	{
		size_t pos = i * accessor.stride;
		return {ReadFloat(accessor.data, pos), ReadFloat(accessor.data, pos + 4), ReadFloat(accessor.data, pos + 8)};
	}

	void AddNode(size_t index, const Transform &parent, int depth)
	{
		if (depth > 32) throw std::string("nodes are nested too deep");
		const nlohmann::json &node = this->json.at("nodes").at(index);

		Transform local{{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
		if (node.contains("matrix")) {
			/* Sixteen numbers, column by column. */
			auto m = node.at("matrix").get<std::vector<float>>();
			if (m.size() != 16) throw std::string("bad node matrix");
			local.m = {m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]};
			local.t = {m[12], m[13], m[14]};
		} else {
			auto t = node.value("translation", std::vector<float>{0, 0, 0});
			auto q = node.value("rotation", std::vector<float>{0, 0, 0, 1});
			auto s = node.value("scale", std::vector<float>{1, 1, 1});
			if (t.size() != 3 || q.size() != 4 || s.size() != 3) throw std::string("bad node transformation");
			float x = q[0], y = q[1], z = q[2], w = q[3];
			local.m = {
				(1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y - z * w) * s[1], 2 * (x * z + y * w) * s[2],
				2 * (x * y + z * w) * s[0], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z - x * w) * s[2],
				2 * (x * z - y * w) * s[0], 2 * (y * z + x * w) * s[1], (1 - 2 * (x * x + y * y)) * s[2],
			};
			local.t = {t[0], t[1], t[2]};
		}
		Transform transform = parent * local;

		if (node.contains("mesh")) {
			const nlohmann::json &mesh = this->json.at("meshes").at(node.at("mesh").get<size_t>());
			for (const auto &primitive : mesh.at("primitives")) this->AddPrimitive(primitive, transform);
		}
		if (node.contains("children")) {
			for (const auto &child : node.at("children")) this->AddNode(child.get<size_t>(), transform, depth + 1);
		}
	}

	/**
	 * Find the separate pieces of glass or lamp in the triangles that were just added: triangles that share a corner are one piece.
	 * Each window gets a number, so the game can light them one by one; each lamp is noted with its place, for the light it casts.
	 * @param first The first of the new corners.
	 * @param first_index The first of the new indices.
	 * @param lamps Whether the pieces are lamps rather than windows.
	 */
	void AddGlowingPieces(uint32_t first, size_t first_index, bool lamps)
	{
		size_t count = this->model.vertices.size() - first;
		std::vector<uint32_t> piece(count);
		std::iota(piece.begin(), piece.end(), 0);
		auto find = [&](uint32_t i) {
			while (piece[i] != i) i = piece[i] = piece[piece[i]];
			return i;
		};
		/* Corners at the same place are the same corner. */
		std::map<std::array<int, 3>, uint32_t> places;
		for (uint32_t i = 0; i < count; i++) {
			const ModelVertex &v = this->model.vertices[first + i];
			auto [it, inserted] = places.try_emplace({static_cast<int>(std::lround(v.x * 64)), static_cast<int>(std::lround(v.y * 64)), static_cast<int>(std::lround(v.z * 64))}, i);
			if (!inserted) piece[find(i)] = find(it->second);
		}
		for (size_t i = first_index; i + 2 < this->model.indices.size(); i += 3) {
			uint32_t a = find(this->model.indices[i] - first);
			piece[find(this->model.indices[i + 1] - first)] = a;
			piece[find(this->model.indices[i + 2] - first)] = a;
		}

		struct Box {
			Vec3 min{FLT_MAX, FLT_MAX, FLT_MAX};
			Vec3 max{-FLT_MAX, -FLT_MAX, -FLT_MAX};
		};
		std::map<uint32_t, Box> boxes;
		for (uint32_t i = 0; i < count; i++) {
			const ModelVertex &v = this->model.vertices[first + i];
			Box &box = boxes[find(i)];
			box.min = {std::min(box.min.x, v.x), std::min(box.min.y, v.y), std::min(box.min.z, v.z)};
			box.max = {std::max(box.max.x, v.x), std::max(box.max.y, v.y), std::max(box.max.z, v.z)};
		}
		if (lamps) {
			for (const auto &[root, box] : boxes) this->model.lamps.push_back((box.min + box.max) * 0.5f);
			return;
		}
		for (uint32_t i = 0; i < count; i++) {
			ModelVertex &v = this->model.vertices[first + i];
			const Box &box = boxes[find(i)];
			Vec3 size = box.max - box.min;
			if (std::max({size.x, size.y, size.z}) > 3.2f) {
				v.a = ModelVertex::GLASS;
				continue;
			}
			/* Windows are numbered by where they are, so the same model always gets the same numbers. */
			uint32_t number = static_cast<uint32_t>(std::lround(box.min.x * 7) * 3 + std::lround(box.min.y * 7) * 5 + std::lround(box.min.z * 7) * 11) + this->windows;
			v.a = static_cast<uint8_t>(ModelVertex::WINDOW + number % ModelVertex::NUM_WINDOWS * 8);
		}
		this->windows += 3;
	}

	void AddPrimitive(const nlohmann::json &primitive, const Transform &transform)
	{
		if (primitive.value("mode", 4) != 4) return; // Only triangles.
		const nlohmann::json &attributes = primitive.at("attributes");
		if (!attributes.contains("POSITION")) return;

		Accessor positions = this->GetAccessor(attributes.at("POSITION").get<size_t>(), "VEC3", 3);
		if (positions.component_type != 5126) throw std::string("positions must be floats");
		bool has_normals = attributes.contains("NORMAL");
		Accessor normals{};
		if (has_normals) {
			normals = this->GetAccessor(attributes.at("NORMAL").get<size_t>(), "VEC3", 3);
			if (normals.component_type != 5126 || normals.count != positions.count) throw std::string("bad normals");
		}

		/* The colour of the material. glTF has linear colours; the screen has not. */
		std::array<uint8_t, 4> colour{200, 200, 200, 255};
		bool is_load = false;
		int texture = -1;
		Accessor coordinates{};
		bool casts_light = false;
		if (primitive.contains("material")) {
			const nlohmann::json &material = this->json.at("materials").at(primitive.at("material").get<size_t>());
			std::vector<float> factor{1, 1, 1, 1};
			if (material.contains("pbrMetallicRoughness")) {
				const nlohmann::json &surface = material.at("pbrMetallicRoughness");
				factor = surface.value("baseColorFactor", factor);
				/* Only textures that use the first set of coordinates, as floats. */
				if (surface.contains("baseColorTexture") && surface.at("baseColorTexture").value("texCoord", 0) == 0 && attributes.contains("TEXCOORD_0")) {
					coordinates = this->GetAccessor(attributes.at("TEXCOORD_0").get<size_t>(), "VEC2", 2);
					if (coordinates.component_type == 5126 && coordinates.count == positions.count) texture = this->GetTexture(surface.at("baseColorTexture").at("index").get<size_t>());
				}
			}
			if (factor.size() != 4) throw std::string("bad material colour");
			for (int i = 0; i < 3; i++) colour[i] = static_cast<uint8_t>(std::clamp(std::pow(std::max(factor[i], 0.0f), 1 / 2.2f) * 255.0f + 0.5f, 0.0f, 255.0f));
			std::string name = material.value("name", std::string{});
			if (name.starts_with("company")) colour[3] = 0;
			/* Leaves change with the seasons, but not those of palms, jungle trees and cacti; ModelPack sorts out the dark ones, which are mostly firs. */
			auto has = [&name](std::string_view part) { return name.find(part) != std::string::npos; };
			if ((has("leaf") || has("leaves") || has("foliage")) && !has("palm") && !has("jungle") && !has("dry")) {
				colour[3] = has("dark") || has("blue") ? ModelVertex::LEAF_EVERGREEN : ModelVertex::LEAF;
			}
			is_load = name.starts_with("load");
			/* What glows at night goes by the name of the material as well. */
			if (name.find("glass") != std::string::npos) {
				colour[3] = ModelVertex::WINDOW;
			} else if (name == "light" || name.ends_with("_lamp") || name.ends_with("_on") || name.ends_with("_glow") || name.ends_with("ufo_light")) {
				colour[3] = ModelVertex::LAMP;
				casts_light = name == "light" || name.ends_with("_lamp");
			}
		}

		/* From glTF axes to the frame of our models: front, left, up. */
		auto convert = [](const std::array<float, 3> &p) { return std::array<float, 3>{p[2], p[0], p[1]}; };

		if (this->model.vertices.size() + positions.count > 1000000) throw std::string("model has too many corners");
		uint32_t first = static_cast<uint32_t>(this->model.vertices.size());
		for (size_t i = 0; i < positions.count; i++) {
			std::array<float, 3> p = convert(transform.Point(this->ReadVec3(positions, i)));
			std::array<float, 3> n{0, 0, 0};
			/* Good enough for rotations and even scaling, which is what models use. */
			if (has_normals) n = convert(transform.Direction(this->ReadVec3(normals, i)));
			float u = 0, v = 0;
			if (texture >= 0) {
				u = ReadFloat(coordinates.data, i * coordinates.stride);
				v = ReadFloat(coordinates.data, i * coordinates.stride + 4);
			}
			this->model.vertices.push_back({p[0], p[1], p[2], n[0], n[1], n[2], colour[0], colour[1], colour[2], colour[3], u, v, static_cast<float>(texture)});
		}

		size_t first_index = this->model.indices.size();
		if (primitive.contains("indices")) {
			Accessor indices = this->GetAccessor(primitive.at("indices").get<size_t>(), "SCALAR", 1);
			for (size_t i = 0; i < indices.count; i++) {
				size_t pos = i * indices.stride;
				uint32_t value;
				switch (indices.component_type) {
					case 5121: value = indices.data[pos]; break;
					case 5123: value = indices.data[pos] | indices.data[pos + 1] << 8; break;
					case 5125: value = Read32(indices.data, pos); break;
					default: throw std::string("bad index format");
				}
				if (value >= positions.count) throw std::string("triangle refers to a missing corner");
				this->model.indices.push_back(first + value);
			}
		} else {
			for (size_t i = 0; i < positions.count; i++) this->model.indices.push_back(first + static_cast<uint32_t>(i));
		}
		/* Whole triangles only. */
		this->model.indices.resize(first_index + (this->model.indices.size() - first_index) / 3 * 3);

		if (!has_normals) {
			/* Each corner gets the direction of the last triangle that uses it. */
			for (size_t i = first_index; i + 2 < this->model.indices.size(); i += 3) {
				ModelVertex &a = this->model.vertices[this->model.indices[i]];
				ModelVertex &b = this->model.vertices[this->model.indices[i + 1]];
				ModelVertex &c = this->model.vertices[this->model.indices[i + 2]];
				Vec3 n = Cross(Vec3{b.x - a.x, b.y - a.y, b.z - a.z}, Vec3{c.x - a.x, c.y - a.y, c.z - a.z});
				for (ModelVertex *v : {&a, &b, &c}) {
					v->nx = n.x;
					v->ny = n.y;
					v->nz = n.z;
				}
			}
		}

		if (colour[3] == ModelVertex::WINDOW || casts_light) this->AddGlowingPieces(first, first_index, casts_light);

		if (is_load) {
			this->load_indices.insert(this->load_indices.end(), this->model.indices.begin() + first_index, this->model.indices.end());
			this->model.indices.resize(first_index);
		}

		for (size_t i = first; i < this->model.vertices.size(); i++) {
			ModelVertex &v = this->model.vertices[i];
			Vec3 n = Normalise(Vec3{v.nx, v.ny, v.nz});
			if (n.x == 0 && n.y == 0 && n.z == 0) n.z = 1;
			v.nx = n.x;
			v.ny = n.y;
			v.nz = n.z;
		}
	}
};

} // namespace

/**
 * Read a model from the contents of a binary glTF file.
 * @param data The contents of the file.
 * @param[out] model The model.
 * @param[out] error What is wrong with the file, when it could not be read.
 * @param find_texture Finds the textures of the model; without it the model gets none.
 * @return Whether the model was read.
 */
bool LoadGlbModel(std::span<const uint8_t> data, Model3D &model, std::string &error, const ModelTextureFinder &find_texture)
{
	model = {};
	try {
		GlbReader reader(data, model, find_texture);
		return true;
	} catch (const std::string &e) {
		error = e;
	} catch (const nlohmann::json::exception &e) {
		error = e.what();
	}
	model = {};
	return false;
}

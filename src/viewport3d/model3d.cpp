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

#include "../safeguards.h"

/*
 * What is read from a binary glTF 2.0 file (.glb):
 * - the triangles of all meshes of the default scene, with the transformations of their nodes;
 * - positions, normals (made from the triangles when missing) and indices;
 * - the base colour of the material as the colour of the triangles.
 * A material with a name that starts with "company" takes the colour of the owner, multiplied with its base colour.
 * Textures, animations and skins are ignored.
 *
 * glTF has x to the left of the model, y up and z to its front. One unit is one world unit: a tile is 16 long.
 */

namespace {

/** Reads a glTF file; errors are reported by throwing a std::string. */
class GlbReader {
public:
	GlbReader(std::span<const uint8_t> data, Model3D &model) : model(model)
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
		if (this->model.indices.empty()) throw std::string("no triangles found");
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
		if (primitive.contains("material")) {
			const nlohmann::json &material = this->json.at("materials").at(primitive.at("material").get<size_t>());
			std::vector<float> factor{1, 1, 1, 1};
			if (material.contains("pbrMetallicRoughness")) factor = material.at("pbrMetallicRoughness").value("baseColorFactor", factor);
			if (factor.size() != 4) throw std::string("bad material colour");
			for (int i = 0; i < 3; i++) colour[i] = static_cast<uint8_t>(std::clamp(std::pow(std::max(factor[i], 0.0f), 1 / 2.2f) * 255.0f + 0.5f, 0.0f, 255.0f));
			if (material.value("name", std::string{}).starts_with("company")) colour[3] = 0;
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
			this->model.vertices.push_back({p[0], p[1], p[2], n[0], n[1], n[2], colour[0], colour[1], colour[2], colour[3]});
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
 * @return Whether the model was read.
 */
bool LoadGlbModel(std::span<const uint8_t> data, Model3D &model, std::string &error)
{
	model.vertices.clear();
	model.indices.clear();
	try {
		GlbReader reader(data, model);
		return true;
	} catch (const std::string &e) {
		error = e;
	} catch (const nlohmann::json::exception &e) {
		error = e.what();
	}
	model.vertices.clear();
	model.indices.clear();
	return false;
}

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file camera3d.h Orbit camera and the small amount of matrix maths the 3D viewport needs. */

#ifndef VIEWPORT3D_CAMERA3D_H
#define VIEWPORT3D_CAMERA3D_H

#include <array>
#include <cmath>

/**
 * 4x4 matrix in column-major memory layout (translation in elements 12..14),
 * using the column-vector convention: a point is transformed as M * p.
 */
struct Matrix4 {
	std::array<float, 16> m{};

	/** Matrix product: (*this) * other. */
	Matrix4 operator*(const Matrix4 &o) const
	{
		Matrix4 r;
		for (int c = 0; c < 4; c++) {
			for (int row = 0; row < 4; row++) {
				float sum = 0;
				for (int k = 0; k < 4; k++) sum += this->m[k * 4 + row] * o.m[c * 4 + k];
				r.m[c * 4 + row] = sum;
			}
		}
		return r;
	}
};

/** Simple 3D vector in world units (16 units per tile, 8 units per height level). */
struct Vec3 {
	float x = 0, y = 0, z = 0;

	Vec3 operator+(const Vec3 &o) const { return {this->x + o.x, this->y + o.y, this->z + o.z}; }
	Vec3 operator-(const Vec3 &o) const { return {this->x - o.x, this->y - o.y, this->z - o.z}; }
	Vec3 operator*(float f) const { return {this->x * f, this->y * f, this->z * f}; }
};

inline float Dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3 &a, const Vec3 &b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 Normalise(const Vec3 &v)
{
	float len = std::sqrt(Dot(v, v));
	return len > 0 ? v * (1.0f / len) : v;
}

/**
 * Camera orbiting a point on the map.
 * The world is right-handed: x runs to the south-west, y to the south-east and z up,
 * exactly like OpenTTD's own world coordinates.
 */
struct Camera3D {
	static constexpr float FOV_Y = 0.6f; ///< Vertical field of view in radians.
	static constexpr float MIN_PITCH = 0.15f; ///< Lowest allowed angle above the horizon in radians.
	static constexpr float MAX_PITCH = 1.50f; ///< Highest allowed angle above the horizon in radians.
	static constexpr float MIN_DISTANCE = 48.0f; ///< Closest distance to the target in world units.
	static constexpr float MAX_DISTANCE = 16384.0f; ///< Furthest distance to the target in world units.

	Vec3 target{}; ///< Point the camera looks at.
	float yaw = 0.7853982f; ///< Direction from the target to the camera around the z axis; the default looks north like the 2D view.
	float pitch = 0.5235988f; ///< Angle of the camera above the horizon; the default of 30 degrees matches the 2D view.
	float distance = 640.0f; ///< Distance from the target to the camera.

	/** Position of the camera. */
	Vec3 GetEye() const
	{
		float cp = std::cos(this->pitch);
		return this->target + Vec3{cp * std::cos(this->yaw), cp * std::sin(this->yaw), std::sin(this->pitch)} * this->distance;
	}

	/** Unit vector pointing to the right of the screen, parallel to the ground. */
	Vec3 GetRight() const { return {-std::sin(this->yaw), std::cos(this->yaw), 0}; }

	/** Unit vector pointing away from the camera, parallel to the ground. */
	Vec3 GetForward() const { return {-std::cos(this->yaw), -std::sin(this->yaw), 0}; }

	float GetNear() const { return std::max(4.0f, this->distance * 0.05f); }
	float GetFar() const { return this->distance * 6.0f + 2048.0f; }

	/** World to camera space; the camera looks down its negative z axis. */
	Matrix4 GetViewMatrix() const
	{
		Vec3 eye = this->GetEye();
		Vec3 f = Normalise(this->target - eye);
		Vec3 s = Normalise(Cross(f, Vec3{0, 0, 1}));
		Vec3 u = Cross(s, f);
		Matrix4 r;
		r.m = {
			s.x, u.x, -f.x, 0,
			s.y, u.y, -f.y, 0,
			s.z, u.z, -f.z, 0,
			-Dot(s, eye), -Dot(u, eye), Dot(f, eye), 1,
		};
		return r;
	}

	/**
	 * Camera to clip space.
	 * @param aspect Width divided by height of the image.
	 * @param homogeneous_depth Whether clip space depth runs from -1 to 1 (OpenGL) instead of 0 to 1.
	 */
	Matrix4 GetProjectionMatrix(float aspect, bool homogeneous_depth) const
	{
		float n = this->GetNear();
		float f = this->GetFar();
		float h = 1.0f / std::tan(FOV_Y / 2);
		Matrix4 r;
		r.m[0] = h / aspect;
		r.m[5] = h;
		r.m[11] = -1;
		if (homogeneous_depth) {
			r.m[10] = (f + n) / (n - f);
			r.m[14] = 2 * f * n / (n - f);
		} else {
			r.m[10] = f / (n - f);
			r.m[14] = n * f / (n - f);
		}
		return r;
	}
};

/** View frustum for culling, built from a combined projection * view matrix. */
struct Frustum3D {
	std::array<std::array<float, 4>, 5> planes{}; ///< Left, right, bottom, top and far plane; the near plane is not needed.

	explicit Frustum3D(const Matrix4 &view_proj)
	{
		auto row = [&](int i) { return std::array<float, 4>{view_proj.m[i], view_proj.m[4 + i], view_proj.m[8 + i], view_proj.m[12 + i]}; };
		auto combine = [](const std::array<float, 4> &a, const std::array<float, 4> &b, float sign) {
			return std::array<float, 4>{a[0] + sign * b[0], a[1] + sign * b[1], a[2] + sign * b[2], a[3] + sign * b[3]};
		};
		this->planes[0] = combine(row(3), row(0), 1);
		this->planes[1] = combine(row(3), row(0), -1);
		this->planes[2] = combine(row(3), row(1), 1);
		this->planes[3] = combine(row(3), row(1), -1);
		this->planes[4] = combine(row(3), row(2), -1);
	}

	/** Test whether an axis aligned box is at least partly inside the frustum. */
	bool IntersectsBox(const Vec3 &min, const Vec3 &max) const
	{
		for (const auto &p : this->planes) {
			float d = p[0] * (p[0] > 0 ? max.x : min.x) + p[1] * (p[1] > 0 ? max.y : min.y) + p[2] * (p[2] > 0 ? max.z : min.z) + p[3];
			if (d < 0) return false;
		}
		return true;
	}
};

#endif /* VIEWPORT3D_CAMERA3D_H */

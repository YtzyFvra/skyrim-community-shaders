#pragma once

// Ring of HMD poses, one per engine frame, and the half-frame interpolation the
// compositor needs to reproject an interpolated frame correctly (vr-frame-generation.md).

#include <openvr.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace VrFrameGen
{
	struct PoseSample
	{
		vr::HmdMatrix34_t deviceToAbsolute{};
		std::uint64_t frameId = 0;
		std::int64_t qpc = 0;
		bool valid = false;
	};

	class PoseRing
	{
	public:
		/** Records the pose the engine received from WaitGetPoses for this frame. */
		void Push(const vr::HmdMatrix34_t& pose, std::uint64_t frameId, std::int64_t qpc)
		{
			auto& slot = samples_[write_ % kSlots];
			slot.deviceToAbsolute = pose;
			slot.frameId = frameId;
			slot.qpc = qpc;
			slot.valid = true;
			++write_;
		}

		/** Pose of the most recent frame (N). */
		const PoseSample& Latest() const { return samples_[(write_ + kSlots - 1) % kSlots]; }
		/** Pose of the frame before it (N-1). */
		const PoseSample& Previous() const { return samples_[(write_ + kSlots - 2) % kSlots]; }
		bool HasPair() const { return write_ >= 2 && Latest().valid && Previous().valid; }

		/** Pose for the frame interpolated between N-1 and N; t = 0.5 for a single mid frame. */
		vr::HmdMatrix34_t Interpolated(float t) const
		{
			return Interpolate(Previous().deviceToAbsolute, Latest().deviceToAbsolute, t);
		}

		/** Slerp rotation, lerp translation. Rotation matrix rows are the upper 3×3 of HmdMatrix34_t. */
		static vr::HmdMatrix34_t Interpolate(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b, float t)
		{
			std::array<float, 4> qa = ToQuaternion(a);
			std::array<float, 4> qb = ToQuaternion(b);
			float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
			if (dot < 0.0f) {
				for (float& v : qb)
					v = -v;
				dot = -dot;
			}
			std::array<float, 4> q{};
			if (dot > 0.9995f) {
				for (int i = 0; i < 4; ++i)
					q[i] = qa[i] + (qb[i] - qa[i]) * t;
			} else {
				const float theta = std::acos(dot);
				const float sinTheta = std::sin(theta);
				const float wa = std::sin((1.0f - t) * theta) / sinTheta;
				const float wb = std::sin(t * theta) / sinTheta;
				for (int i = 0; i < 4; ++i)
					q[i] = qa[i] * wa + qb[i] * wb;
			}
			const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			for (float& v : q)
				v /= length;
			vr::HmdMatrix34_t out = FromQuaternion(q);
			for (int row = 0; row < 3; ++row)
				out.m[row][3] = a.m[row][3] + (b.m[row][3] - a.m[row][3]) * t;
			return out;
		}

	private:
		static constexpr std::size_t kSlots = 4;

		static std::array<float, 4> ToQuaternion(const vr::HmdMatrix34_t& m)
		{
			// (x, y, z, w)
			const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
			std::array<float, 4> q{};
			if (trace > 0.0f) {
				const float s = std::sqrt(trace + 1.0f) * 2.0f;
				q = { (m.m[2][1] - m.m[1][2]) / s, (m.m[0][2] - m.m[2][0]) / s, (m.m[1][0] - m.m[0][1]) / s, 0.25f * s };
			} else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
				const float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;
				q = { 0.25f * s, (m.m[0][1] + m.m[1][0]) / s, (m.m[0][2] + m.m[2][0]) / s, (m.m[2][1] - m.m[1][2]) / s };
			} else if (m.m[1][1] > m.m[2][2]) {
				const float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;
				q = { (m.m[0][1] + m.m[1][0]) / s, 0.25f * s, (m.m[1][2] + m.m[2][1]) / s, (m.m[0][2] - m.m[2][0]) / s };
			} else {
				const float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;
				q = { (m.m[0][2] + m.m[2][0]) / s, (m.m[1][2] + m.m[2][1]) / s, 0.25f * s, (m.m[1][0] - m.m[0][1]) / s };
			}
			return q;
		}

		static vr::HmdMatrix34_t FromQuaternion(const std::array<float, 4>& q)
		{
			const float x = q[0], y = q[1], z = q[2], w = q[3];
			vr::HmdMatrix34_t m{};
			m.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
			m.m[0][1] = 2.0f * (x * y - z * w);
			m.m[0][2] = 2.0f * (x * z + y * w);
			m.m[1][0] = 2.0f * (x * y + z * w);
			m.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
			m.m[1][2] = 2.0f * (y * z - x * w);
			m.m[2][0] = 2.0f * (x * z - y * w);
			m.m[2][1] = 2.0f * (y * z + x * w);
			m.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
			return m;
		}

		std::array<PoseSample, kSlots> samples_{};
		std::uint64_t write_ = 0;
	};
}

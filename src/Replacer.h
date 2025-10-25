#pragma once

#include "ConditionParser.h"

namespace PAR
{
	struct Override
	{
		std::string name;
		RE::NiTransform transform;
	};

	typedef std::vector<Override> Frame;
	typedef std::set<std::reference_wrapper<const std::string>, std::less<std::string>> BoneSet;

	struct Clamp
	{
		std::string name;
		std::array<float, 3> rotate_low;   // Euler angles
		std::array<float, 3> rotate_high;  //
		std::array<float, 3> translate_low;
		std::array<float, 3> translate_high;
		float scale_low;
		float scale_high;
	};

	struct ReplacerData
	{
		uint64_t priority;
		std::vector<Frame> frames;
		std::vector<Clamp> clamps;

		bool rotate;
		bool translate;
		bool scale;

		std::vector<std::string> conditions;
		std::unordered_map<std::string, std::string> refs;
	};

	class Replacer
	{
	public:
		Replacer(const ReplacerData& a_raw) :
			_priority(a_raw.priority), _frames(a_raw.frames), _clamps(a_raw.clamps), _rotate(a_raw.rotate), _translate(a_raw.translate), _scale(a_raw.scale)
		{
			for (const auto& [key, ref] : a_raw.refs) {
				_refs[key] = Util::GetFormFromString(ref);
			}

			auto condition = std::make_shared<RE::TESCondition>();
			RE::TESConditionItem** head = std::addressof(condition->head);
			int numConditions = 0;
			for (auto& text : a_raw.conditions) {
				if (text.empty())
					continue;

				if (auto conditionItem = ConditionParser::Parse(text, _refs)) {
					*head = conditionItem;
					head = std::addressof(conditionItem->next);
					numConditions += 1;
				} else {
					logger::info("Aborting condition parsing"sv);
					numConditions = 0;
					break;
				}
			}

			_conditions = numConditions ? condition : nullptr;

			// Build boneset.
			// To avoid looping over all frames, assume no frame contains other bones than the first frame.
			// In the future, if elaborate multi-frame PAR replacements appear, you may want to change this.
			if (not _frames.empty()) {
				for (const auto& override : _frames[0]) {
					_boneset.insert(override.name);
				}
			}
			for (const auto& clamp : _clamps) {
				_boneset.insert(clamp.name);
			}
		}
		inline ReplacerData GetData()
		{
			return ReplacerData{ _priority, _frames, _clamps, _rotate, _translate, _scale };
		}

		// Within 3% error of tanh if -4 < x < 4. Overshoots slightly beyond that!
		static float FastTanh(float x)
		{
			const float x2 = x * x;
			return x * (27 + x2) / (27 + 9 * x2);
		}

		static float Saturate(float x, float lo, float hi)
		{
			if (lo == hi)
				return x;
			const float s = (hi - lo) / 2;
			const float m = (hi + lo) / 2;
			return m + s * std::tanh((x - m) / s);
		}

		inline void Apply(RE::NiAVObject* a_obj) const
		{
			// Apply frame
			if (not _frames.empty()) {
				const auto& overrides = _frames[0];  // no multi-frame support as of now

				for (const auto& override : overrides) {
					if (const auto node = a_obj->GetObjectByName(override.name)) {
						if (_rotate) {
							node->local.rotate = override.transform.rotate;
						}
						if (_translate) {
							node->local.translate = override.transform.translate;
						}
						if (_scale) {
							node->local.scale = override.transform.scale;
						}
					}
				}
			}
			// Apply clamps
			for (const auto& clamp : _clamps) {
				if (const auto node = a_obj->GetObjectByName(clamp.name)) {
					if (_rotate) {
						RE::NiPoint3 eulers;
						node->local.rotate.ToEulerAnglesXYZ(eulers);
						for (int i = 0; i < 3; ++i) {
							eulers[i] = Saturate(eulers[i], clamp.rotate_low[i], clamp.rotate_high[i]);
							node->local.rotate.SetEulerAnglesXYZ(-eulers.x, eulers.y, -eulers.z);
						}
					}
					if (_translate) {
						for (int i = 0; i < 3; ++i) {
							node->local.translate[i] = Saturate(node->local.translate[i], clamp.translate_low[i], clamp.translate_high[i]);
						}
					}
					if (_scale) {
						node->local.scale = Saturate(node->local.scale, clamp.scale_low, clamp.scale_high);
					}
				}
			}
		}

		inline bool Eval(RE::Actor* a_actor) const
		{
			return _conditions != nullptr && _conditions->IsTrue(a_actor, a_actor);
		}
		inline bool IsValid(const std::string& a_file) const
		{
			bool valid = true;

			if (!_conditions) {
				logger::error("{}: must have conditions", a_file);
			}

			if (_frames.empty() and _clamps.empty()) {
				logger::error("{}: no frames nor clamps found", a_file);
				valid = false;
			}

			for (int i = 0; i < _frames.size(); i++) {
				const auto& frame = _frames[i];
				if (frame.empty()) {
					logger::error("{}: no overrides defined in frame at {}", a_file, i);
					valid = false;
				}
				for (const auto& override : frame) {
					if (override.name.empty()) {
						logger::error("{}: override with no node found in frame at {}", a_file, i);
						valid = false;
						break;
					}
				}
			}
			for (const auto& clamp : _clamps) {
				if (clamp.name.empty()) {
					logger::error("{}: clamp with no node found", a_file);
					valid = false;
					break;
				}
			}

			return valid;
		}
		uint64_t GetPriority() const { return _priority; }
		const BoneSet& GetBoneset() const { return _boneset; }

	private:
		uint64_t _priority;
		std::vector<std::vector<Override>> _frames;
		std::vector<Clamp> _clamps;

		bool _rotate;
		bool _translate;
		bool _scale;

		std::shared_ptr<RE::TESCondition> _conditions = nullptr;
		ConditionParser::RefMap _refs;
		BoneSet _boneset;  // set of bones overriden by this replacer.
	};

	inline void from_json(const json& j, Override& o)
	{
		o.name = j.value("name", "");

		// TODO: make the fields in frame optional. This will crash if rotate is not provided. 
		// const std::vector<std::vector<float>> Identity{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
		// const auto val = j.value("rotate", Identity);
		const auto val = j.value("rotate", std::vector<std::vector<float>>{});

		for (int i = 0; i < 3; i++) {
			for (int k = 0; k < 3; k++) {
				o.transform.rotate.entry[i][k] = val[i][k];
			}
		}

		o.transform.translate = RE::NiPoint3{
			j["translate"].value("x", 0.f),
			j["translate"].value("y", 0.f),
			j["translate"].value("z", 0.f)
		};

		o.transform.scale = j.value("scale", 1.f);
	}

	inline void to_json(json& j, const Override& o)
	{
		j = json{
			{ "name", o.name },
			{ "rotate", o.transform.rotate.entry },
			{ "translate",
				json{
					{ "x", o.transform.translate.x },
					{ "y", o.transform.translate.y },
					{ "z", o.transform.translate.z },
				} },
			{ "scale", o.transform.scale }
		};
	}

	inline void from_json(const json& j, Clamp& c)
	{
		constexpr std::array<float, 3> zeros = std::array<float, 3>{ 0.f, 0.f, 0.f };
		c.name = j.value("name", "");
		c.rotate_low = j.value("rotate_low", zeros);
		c.rotate_high = j.value("rotate_high", zeros);
		c.translate_low = j.value("translate_low", zeros);
		c.translate_high = j.value("translate_high", zeros);
		c.scale_low = j.value("scale_low", 0.f);
		c.scale_high = j.value("scale_high", 0.f);
		// convert degrees to radians
		for (int i = 0; i < 3; ++i) {
			c.rotate_low[i] = RE::deg_to_rad(c.rotate_low[i]);
			c.rotate_high[i] = RE::deg_to_rad(c.rotate_high[i]);
		}
	}

	inline void to_json(json& j, const Clamp& c)
	{
		// convert degrees to radians
		std::array<float, 3> rotate_low_deg;
		std::array<float, 3> rotate_high_deg;
		for (int i = 0; i < 3; ++i) {
			rotate_low_deg[i] = RE::rad_to_deg(c.rotate_low[i]);
			rotate_high_deg[i] = RE::rad_to_deg(c.rotate_high[i]);
		}

		j = json{
			{ "name", c.name },
			{ "rotate_low", rotate_low_deg },
			{ "rotate_high", rotate_high_deg },
			{ "translate_low", c.translate_low },
			{ "translate_high", c.translate_high },
			{ "scale_low", c.scale_low },
			{ "scale_high", c.scale_high }
		};
	}

	inline void from_json(const json& j, ReplacerData& r)
	{
		r.priority = j.value("priority", 0);
		r.frames = j.value("frames", std::vector<Frame>{});
		r.clamps = j.value("clamps", std::vector<Clamp>{});
		r.conditions = j.value("conditions", std::vector<std::string>{});
		r.refs = j.value("refs", std::unordered_map<std::string, std::string>{});
		r.rotate = j.value("rotate", true);
		r.translate = j.value("translate", false);
		r.scale = j.value("scale", false);
	}

	inline void to_json(json& j, const ReplacerData& r)
	{
		j = json{
			{ "priority", r.priority },
			{ "conditions", r.conditions },
			{ "rotate", r.rotate },
			{ "translate", r.translate },
			{ "scale", r.scale },
			{ "refs", r.refs },
			{ "frames", r.frames },
			{ "clamps", r.clamps }
		};
	}
}
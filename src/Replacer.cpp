#include "Replacer.h"

namespace PAR
{
	Replacer::Replacer(const ReplacerData& a_raw) :
		_priority(a_raw.priority),
		_frames(a_raw.frames),
		_limits(a_raw.limits),
		_rotate(a_raw.rotate),
		_translate(a_raw.translate),
		_scale(a_raw.scale)
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

		if (not _frames.empty()) {
			for (const auto& override : _frames[0]) {
				_boneset.insert(override.name);
			}
		}

		for (const auto& lim : _limits) {
			_boneset.insert(lim.name);
		}
	}

	ReplacerData Replacer::GetData()
	{
		return ReplacerData{ _priority, _frames, _limits, _rotate, _translate, _scale };
	}

	float Replacer::FastTanh(float x)
	{
		const float x2 = x * x;
		return x * (27 + x2) / (27 + 9 * x2);
	}

	float Replacer::Saturate(float x, float lo, float hi)
	{
		if (lo == hi)
			return x;
		const float s = (hi - lo) / 2;
		const float m = (hi + lo) / 2;
		return m + s * std::tanh((x - m) / s);
	}

	void Replacer::Apply(RE::NiAVObject* a_obj) const
	{
		if (not _frames.empty()) {
			const auto& overrides = _frames[0];

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

		for (const auto& lim : _limits) {
			if (const auto node = a_obj->GetObjectByName(lim.name)) {
				if (_rotate) {
					RE::NiPoint3 eulers;
					node->local.rotate.ToEulerAnglesXYZ(eulers);
					for (int i = 0; i < 3; ++i) {
						eulers[i] = Saturate(eulers[i], lim.rotate_low[i], lim.rotate_high[i]);
						node->local.rotate.SetEulerAnglesXYZ(-eulers.x, eulers.y, -eulers.z);
					}
				}
				if (_translate) {
					for (int i = 0; i < 3; ++i) {
						node->local.translate[i] = Saturate(node->local.translate[i], lim.translate_low[i], lim.translate_high[i]);
					}
				}
				if (_scale) {
					node->local.scale = Saturate(node->local.scale, lim.scale_low, lim.scale_high);
				}
			}
		}
	}

	bool Replacer::Eval(RE::Actor* a_actor) const
	{
		return _conditions != nullptr && _conditions->IsTrue(a_actor, a_actor);
	}

	bool Replacer::IsValid(const std::string& a_file) const
	{
		bool valid = true;

		if (!_conditions) {
			logger::error("{}: must have conditions", a_file);
		}

		if (_frames.empty() && _limits.empty()) {
			logger::error("{}: no frames nor limits found", a_file);
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

		for (const auto& lim : _limits) {
			if (lim.name.empty()) {
				logger::error("{}: lim with no node found", a_file);
				valid = false;
				break;
			}
		}

		return valid;
	}

	uint64_t Replacer::GetPriority() const
	{
		return _priority;
	}

	const BoneSet& Replacer::GetBoneset() const
	{
		return _boneset;
	}

	// JSON helpers
	void from_json(const json& j, Override& o)
	{
		o.name = j.value("name", "");
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

	void to_json(json& j, const Override& o)
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

	void from_json(const json& j, Limit& c)
	{
		constexpr std::array<float, 3> zeros = std::array<float, 3>{ 0.f, 0.f, 0.f };
		c.name = j.value("name", "");
		c.rotate_low = j.value("rotate_low", zeros);
		c.rotate_high = j.value("rotate_high", zeros);
		c.translate_low = j.value("translate_low", zeros);
		c.translate_high = j.value("translate_high", zeros);
		c.scale_low = j.value("scale_low", 0.f);
		c.scale_high = j.value("scale_high", 0.f);

		// Convert degrees to radians
		for (int i = 0; i < 3; ++i) {
			c.rotate_low[i] = RE::deg_to_rad(c.rotate_low[i]);
			c.rotate_high[i] = RE::deg_to_rad(c.rotate_high[i]);
		}
	}

	void to_json(json& j, const Limit& c)
	{
		// Convert radians to degrees
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

	void from_json(const json& j, ReplacerData& r)
	{
		r.priority = j.value("priority", 0);
		r.frames = j.value("frames", std::vector<Frame>{});
		r.limits = j.value("limits", std::vector<Limit>{});
		r.conditions = j.value("conditions", std::vector<std::string>{});
		r.refs = j.value("refs", std::unordered_map<std::string, std::string>{});
		r.rotate = j.value("rotate", true);
		r.translate = j.value("translate", false);
		r.scale = j.value("scale", false);
	}

	void to_json(json& j, const ReplacerData& r)
	{
		j = json{
			{ "priority", r.priority },
			{ "conditions", r.conditions },
			{ "rotate", r.rotate },
			{ "translate", r.translate },
			{ "scale", r.scale },
			{ "refs", r.refs },
			{ "frames", r.frames },
			{ "limits", r.limits }
		};
	}
}

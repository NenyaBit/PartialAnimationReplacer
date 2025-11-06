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

    struct Limit
    {
        std::string name;
        std::array<float, 3> rotate_low;   
        std::array<float, 3> rotate_high;  
        std::array<float, 3> translate_low;
        std::array<float, 3> translate_high;
        float scale_low;
        float scale_high;
    };

    struct ReplacerData
    {
        uint64_t priority;
        std::vector<Frame> frames;
        std::vector<Limit> limits;

        bool rotate;
        bool translate;
        bool scale;

        std::vector<std::string> conditions;
        std::unordered_map<std::string, std::string> refs;
    };

    class Replacer
    {
    public:
        Replacer(const ReplacerData& a_raw);

        ReplacerData GetData();
        static float FastTanh(float x);
        static float Saturate(float x, float lo, float hi);

        void Apply(RE::NiAVObject* a_obj) const;
        bool Eval(RE::Actor* a_actor) const;
        bool IsValid(const std::string& a_file) const;
        uint64_t GetPriority() const;
        const BoneSet& GetBoneset() const;

    private:
        uint64_t _priority;
        std::vector<std::vector<Override>> _frames;
        std::vector<Limit> _limits;

        bool _rotate;
        bool _translate;
        bool _scale;

        std::shared_ptr<RE::TESCondition> _conditions;
        ConditionParser::RefMap _refs;
        BoneSet _boneset;
    };

    void from_json(const json& j, Override& o);
    void to_json(json& j, const Override& o);

    void from_json(const json& j, Limit& c);
    void to_json(json& j, const Limit& c);

    void from_json(const json& j, ReplacerData& r);
    void to_json(json& j, const ReplacerData& r);
}
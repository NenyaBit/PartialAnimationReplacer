#include "ReplacerManager.h"

using namespace PAR;

void ReplacerManager::EvaluateReplacers()
{
	auto replacers = std::make_shared<ReplacerMap>();

	std::unique_lock lock{ _mutex };

	std::vector<RE::Actor*> actors{ RE::PlayerCharacter::GetSingleton() };
	RE::ProcessLists::GetSingleton()->ForEachHighActor([&actors](RE::Actor* a_actor) {
		if (a_actor->Is3DLoaded()) {
			actors.emplace_back(a_actor);
		}

		return RE::BSContainer::ForEachResult::kContinue;
	});

	for (const auto& actor : actors) {
		FindReplacersForActor(actor, *replacers);
	}
	
	replacers = _current.exchange(replacers);
}

// Evaluates conditions on actor `a_actor` and inserts applicable replacers in `a_map`
void ReplacerManager::FindReplacersForActor(RE::Actor* a_actor, ReplacerMap& a_map)
{
	std::set<std::string> replaced_bones;
	// replacers are already sorted by decreasing priority
	for (const auto& replacer : _replacers) {
		if (replacer->Eval(a_actor)) {
			// test for no shared bones
			const std::set<std::string>& incoming_bones = replacer->GetBoneset();
			std::vector<std::string> common_bones;
			std::set_intersection(replaced_bones.begin(), replaced_bones.end(), incoming_bones.begin(), incoming_bones.end(), std::back_inserter(common_bones)); 
			if (common_bones.empty()) {
				a_map[a_actor->GetFormID()].push_back(replacer);
				replaced_bones.insert(incoming_bones.begin(), incoming_bones.end());
			}
		}
	}
}

void ReplacerManager::ApplyReplacers(RE::NiAVObject* a_playerObj)
{
	if (!_enabled)
		return;

	const auto replacers = _current.load();

	// apply to player
	ApplyReplacersToActor(replacers, 0x14, a_playerObj);

	RE::NiUpdateData updateData{
		0.f,
		RE::NiUpdateData::Flag::kNone
	};

	// apply to NPCs
	RE::ProcessLists::GetSingleton()->ForEachHighActor([&replacers, &updateData](RE::Actor* a_actor) {
		if (const auto obj = a_actor->Get3D(false)) {
			if (ApplyReplacersToActor(replacers, a_actor->GetFormID(), obj)) {
				obj->Update(updateData);
			}
		}
		
		return RE::BSContainer::ForEachResult::kContinue;
	});
}

bool ReplacerManager::ApplyReplacersToActor(const std::shared_ptr<ReplacerMap>& a_map, RE::FormID a_id, RE::NiAVObject* a_obj)
{
	const auto iter = a_map->find(a_id);
	if (iter != a_map->end()) {
		const auto& actorReplacers = iter->second;
		for (const auto& repl : actorReplacers) {
			repl->Apply(a_obj);
		}
		return true;
	}

	return false;
}

void ReplacerManager::Init()
{
	_current = std::make_shared<ReplacerMap>();

	logger::info("ReplacerManager::Init");

	const std::string dir{ "Data\\SKSE\\PartialAnimationReplacer\\Replacers" };
	if (fs::exists(dir)) {
		for (const auto& entry : fs::directory_iterator(dir)) {
			if (!entry.is_directory()) {
				continue;
			}

			LoadDir(entry);
		}
	} else {
		logger::info("replacement dir does not exist");
	}

	Sort();
}

void ReplacerManager::LoadDir(const fs::directory_entry& a_dir)
{
	logger::info("Processing directory {}", a_dir.path().string());
	int found = 0;
	for (const auto& file : fs::directory_iterator(a_dir)) {
		if (file.is_directory())
			continue;

		found += (int)LoadFile(file);
	}
	logger::info("loaded {} replacer from directory {}", found, a_dir.path().string());
}

bool ReplacerManager::ReloadFile(const fs::directory_entry& a_file)
{
	std::unique_lock lock{ _mutex };  // prevent read/writes from replacers
	
	// invalidate current replacers
	auto replacers = std::make_shared<ReplacerMap>();
	replacers = _current.exchange(replacers);

	return LoadFile(a_file);
}

bool ReplacerManager::LoadFile(const fs::directory_entry& a_file)
{
	logger::info("Processing file {}", a_file.path().string());

	const auto ext = a_file.path().extension();

	if (ext != ".json")
		return false;

	const std::string fileName{ a_file.path().string() };

	try {
		logger::info("loading {}", fileName);

		std::ifstream f{ fileName };
		const auto data = json::parse(f);
		const auto replacer = std::make_shared<Replacer>(data.get<ReplacerData>());

		if (replacer->IsValid(fileName)) {
			if (_paths.count(fileName)) {
				_replacers[_paths[fileName]] = replacer;
			} else {
				_paths[fileName] = _replacers.size();
				_replacers.emplace_back(replacer);
			}
		} else if (_paths.count(fileName)) {
			_replacers.erase(_replacers.begin() + _paths[fileName]);
			_paths.erase(fileName);
		}

		Sort();

		return true;
	} catch (std::exception& e) {
		logger::info("failed to load {} - {}", fileName, e.what());

		return false;
	}
}

void ReplacerManager::Sort()
{
	std::ranges::sort(_replacers, [](const auto& a, const auto& b) {
		return a->GetPriority() > b->GetPriority();
	});
}
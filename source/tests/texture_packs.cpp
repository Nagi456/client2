#include "TexturePacks.h"
#include <cassert>
#include <iostream>

using namespace TexturePacks;

static void Write(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out << text;
}

int main()
{
    const std::string manifest = R"({
      "format_version": 2,
      "header": { "name": "Fancy \u30c6\u30af\u30b9\u30c1\u30e3", "description": "x",
                  "uuid": "ABCDEF01-2345-6789-abcd-ef0123456789", "version": [1, 2, 3], "min_engine_version": [1, 21, 0] },
      "modules": [ { "type": "resources", "uuid": "11111111-2222-3333-4444-555555555555", "version": [1, 0, 0] } ]
    })";
    Pack pack;
    assert(ParseManifest(manifest, pack));
    assert(pack.uuid == "abcdef01-2345-6789-abcd-ef0123456789");             // header uuid, lower-cased (not the module's)
    assert(pack.version[0] == 1 && pack.version[1] == 2 && pack.version[2] == 3);
    assert(pack.name == "Fancy \xE3\x83\x86\xE3\x82\xAF\xE3\x82\xB9\xE3\x83\x81\xE3\x83\xA3");   // \u escapes -> UTF-8
    // refused manifests
    Pack other;
    assert(!ParseManifest("not json", other));
    assert(!ParseManifest(R"({"header":{"name":"x","uuid":"abcdef01-2345-6789-abcd-ef0123456789","version":[1,0,0]},"modules":[{"type":"data"}]})", other));  // behaviour pack
    assert(!ParseManifest(R"({"header":{"name":"x","uuid":"short","version":[1,0,0]},"modules":[{"type":"resources"}]})", other));          // bad uuid
    assert(!ParseManifest(R"({"header":{"name":"x","version":[1,0,0]},"modules":[{"type":"resources"}]})", other));                         // no uuid
    // active list
    const auto active = ParseActive(R"([{"pack_id":"ABCDEF01-2345-6789-abcd-ef0123456789","version":[1,2,3]},{"pack_id":"nope"}])");
    assert(active.size() == 1 && active[0] == "abcdef01-2345-6789-abcd-ef0123456789");
    assert(ParseActive("[]").empty() && ParseActive("").empty());
    // written list is read back, and "none" is an empty list
    assert(BuildActive(nullptr) == "[]\n");
    const auto back = ParseActive(BuildActive(&pack));
    assert(back.size() == 1 && back[0] == pack.uuid);
    assert(BuildActive(&pack).find("[1, 2, 3]") != std::string::npos);

    // ---- disk: two data folders, packs in both, newest active file wins ----
    const fs::path base = fs::temp_directory_path() / "texpacks_test";
    fs::remove_all(base);
    const fs::path appData = base / "Roaming", local = base / "Local";
    const fs::path shared = appData / "Minecraft Bedrock" / "Users" / "Shared" / "games" / "com.mojang";
    const fs::path user = appData / "Minecraft Bedrock" / "Users" / "123456" / "games" / "com.mojang";
    Write(shared / "resource_packs" / "Alpha" / "manifest.json", manifest);
    Write(user / "resource_packs" / "pack.beta_folder" / "manifest.json",
          R"({"header":{"name":"pack.name","uuid":"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee","version":[2,0,0]},"modules":[{"type":"resources"}]})");
    Write(user / "resource_packs" / "BehaviourOnly" / "manifest.json",
          R"({"header":{"name":"b","uuid":"bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee","version":[1,0,0]},"modules":[{"type":"data"}]})");
    Write(user / "resource_packs" / "Broken" / "manifest.json", "{{{");
    Write(user / "minecraftpe" / "global_resource_packs.json", BuildActive(&pack));
    const auto roots = CandidateRoots(appData, local);
    assert(roots.size() == 3);   // Shared, 123456, and the old UWP location
    State state = Scan(roots);
    assert(state.found && state.root == user);                                  // the one holding the active list
    assert(state.packs.size() == 2);                                            // behaviour-only and broken ones skipped
    assert(state.packs[0].name == "Fancy \xE3\x83\x86\xE3\x82\xAF\xE3\x82\xB9\xE3\x83\x81\xE3\x83\xA3");
    assert(state.packs[1].name == "pack.beta_folder");                          // "pack.name" is a key -> folder name
    assert(IsActive(state, state.packs[0]) && !IsActive(state, state.packs[1]));

    // switch: the file changes, the original is backed up exactly once
    std::string error;
    assert(WriteActive(state, &state.packs[1], error));
    assert(ParseActive(ReadFile(user / "minecraftpe" / "global_resource_packs.json")) == std::vector<std::string>{"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"});
    const std::string backup = ReadFile(user / "minecraftpe" / "global_resource_packs.json.myclient-backup");
    assert(ParseActive(backup).size() == 1 && ParseActive(backup)[0] == pack.uuid);
    assert(WriteActive(state, nullptr, error));                                 // back to the default textures
    assert(ParseActive(ReadFile(user / "minecraftpe" / "global_resource_packs.json")).empty());
    assert(ReadFile(user / "minecraftpe" / "global_resource_packs.json.myclient-backup") == backup);   // backup kept as the original
    // nothing found
    State none = Scan({base / "does-not-exist"});
    assert(!none.found && !none.note.empty());
    assert(!WriteActive(none, nullptr, error));
    fs::remove_all(base);
    std::cout << "PASS: pack manifests, active list parse/write, data-folder choice, backup, refusal of odd files\n";
}

// NukeTilemap — the grid-world module (Phase 6.4). A SEPARATE module by design: colony
// sims / roguelikes / strategy load it; everyone else never pays for it. See
// include/NukeTilemap/Tilemap.h for the model.
#include <NukeTilemap/Tilemap.h>

#include <interface/NUKEEInteface.h>
#include <interface/AssetCreators.h>
#include <iostream>
#include <cstring>

using namespace nuke;

// nukegen (module mode): reflection registration for the Tilemap component — generated
// into NukeTilemap.gen.inc by the CMake prebuild, #included IN-TU so member pointers
// resolve against the class definition above.
#include "NukeTilemap.gen.inc"   // defines NukeReflectInit_NukeTilemap()

class NukeTilemapModule : public NUKEModule
{
public:
	NukeTilemapModule()
	{
		strcpy(title, "NukeTilemap");
		strcpy(author, "Luastris");
		strcpy(version, "1.0");
		strcpy(description, "Grid worlds: tilemap component, .nutile tile sets, chunked rendering");
	}

	void OnLoad() override
	{
		NukeReflectInit_NukeTilemap();   // Tilemap becomes Add Component-able / scriptable

		// .nutile: a plain-JSON tile-set definition (texture atlas + per-tile defs).
		// Text-editable with JSON highlighting; New > Tiles > Tile Set.
		AssetCreator ac;
		ac.label    = "Tile Set";
		ac.ext      = ".nutile";
		ac.baseName = "New Tile Set";
		ac.category = "Tiles";
		ac.icon     = "î¦¬";   // ICON_LC_GRID_3X3 (raw UTF-8 - no ImGui header needed)
		ac.textEditable   = true;
		ac.syntaxLanguage = "json";
		ac.content =
"{\n"
"  \"texture\": \"tiles.nutex\",\n"
"  \"cols\": 4,\n"
"  \"rows\": 4,\n"
"  \"tiles\": [\n"
"    { \"id\": 1, \"name\": \"grass\",   \"cells\": [0, 1],  \"walk\": 100, \"flags\": [\"buildable\"] },\n"
"    { \"id\": 2, \"name\": \"dirt\",    \"cells\": [2],     \"walk\": 120, \"flags\": [\"buildable\"] },\n"
"    { \"id\": 3, \"name\": \"granite\", \"cells\": [3],     \"walk\": 0,   \"flags\": [\"wall\", \"mine\"] }\n"
"  ]\n"
"}\n";
		RegisterAssetCreator(ac);

		std::cout << "[NukeTilemap]\tloaded" << std::endl;
	}

	void Run(AppInstance*) override {}
	void Shutdown() override { stopped = true; }
	bool HasSettings() override { return false; }
	void Settings() override {}
};

// Exported under the unmangled symbol "plugin" — the loader imports it via boost::dll.
extern "C" __declspec(dllexport) NukeTilemapModule plugin;
NukeTilemapModule plugin;

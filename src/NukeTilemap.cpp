// NukeTilemap module entry point. See include/NukeTilemap/Tilemap.h for the model.
#include <NukeTilemap/Tilemap.h>

#include <interface/NUKEEInteface.h>
#include <interface/AssetCreators.h>
#include <interface/IconsFileTypes.h>
#include <iostream>
#include <cstring>

using namespace nuke;

// nukegen output — must be included IN-TU so member pointers resolve against the class above.
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

		AssetCreator ac;
		ac.label    = "Tile Set";
		ac.ext      = ".nutile";
		ac.baseName = "New Tile Set";
		ac.category = "Tiles";
		ac.icon     = ICON_FT_TILEMAP;
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

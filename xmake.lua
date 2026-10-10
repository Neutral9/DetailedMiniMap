-- CommonLibSSE-NG: the lib/commonlibsse-ng submodule (git submodule update --init --recursive)
includes("lib/commonlibsse-ng")

set_project("DetailedMiniMap")
set_version("1.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")

-- MeshFilter.json
add_requires("nlohmann_json")

-- the target is named after the MO2 mod folder: the plugin rule installs into
-- XSE_TES5_MODS_PATH/<target name>
target("DetailedMiniMap")
    add_rules("commonlibsse-ng.plugin", {
        name = "DetailedMiniMap",
        author = "Neutral9",
        description = "Detailed 3D minimap built from the game's own meshes."
    })

    add_files("src/**.cpp")
    add_packages("nlohmann_json")
    add_headerfiles("src/**.h")
    -- extern: SKSEMenuFramework.h
    add_includedirs("src", "extern")
    set_pcxxheader("src/pch.h")
    add_defines("NOMINMAX")
    -- CommonLib headers: nameless structs, zero-sized arrays, macro redefinitions
    add_cxxflags("cl::/wd4200", "cl::/wd4201", "cl::/wd4005")

    -- the default ini goes next to the dll; the menu saves into DetailedMiniMap_User.ini
    add_installfiles("dist/(SKSE/**)")
    -- MeshFilter.json is the user's to edit: kept apart in dist/defaults, put in place only when there is none yet
    -- (an install never writes over one)
    after_install(function (target)
        local dest = path.join(target:installdir(), "SKSE/Plugins/DetailedMiniMap/MeshFilter.json")
        if not os.isfile(dest) then
            os.cp("dist/defaults/MeshFilter.json", dest)
        end
    end)
    -- the map icons (tools/MakeIcons.java), replaceable
    add_installfiles("dist/(Textures/**)")

    -- xmake install: into the author's MO2 mod folder when it is there (otherwise XSE_TES5_MODS_PATH/DetailedMiniMap)
    on_config(function (target)
        if os.isdir("E:/SOE/mods") then
            target:set("installdir", "E:/SOE/mods/DetailedMiniMap")
        end
    end)

// PresetManager.h — DubSauce preset bank (8 slots, SD card persistence).
// Each preset stores the full 4×8 knob grid (LIVE/SHAPE/DEEP/FX pages).
// Binary file format: 8-byte magic, 8 occupied flags, then 8×4×8 floats.

#pragma once
#include <cstring>
#include "ff.h"

namespace dubsauce
{
static constexpr int PRESET_SLOTS = 8;
static constexpr int PRESET_PAGES = 4;   // sound pages only
static constexpr int PRESET_KNOBS = 8;

class PresetManager
{
  public:
    void Init()
    {
        for(int s = 0; s < PRESET_SLOTS; s++)
            occupied_[s] = false;
    }

    bool HasPreset(int slot) const
    {
        return slot >= 0 && slot < PRESET_SLOTS && occupied_[slot];
    }

    void Save(int slot, const float vals[PRESET_PAGES][PRESET_KNOBS])
    {
        if(slot < 0 || slot >= PRESET_SLOTS) return;
        memcpy(data_[slot], vals, sizeof(float) * PRESET_PAGES * PRESET_KNOBS);
        occupied_[slot] = true;
    }

    bool Load(int slot, float vals[PRESET_PAGES][PRESET_KNOBS]) const
    {
        if(!HasPreset(slot)) return false;
        memcpy(vals, data_[slot], sizeof(float) * PRESET_PAGES * PRESET_KNOBS);
        return true;
    }

    // ---- SD card persistence ----

    bool SaveToSD(FIL& fil, const char* path = "0:/presets.bin")
    {
        UINT bw;
        if(f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;

        // Magic header so we can detect corrupt/wrong files
        static const uint8_t magic[8] = {'D','U','B','S','P','R','S','T'};
        if(f_write(&fil, magic, 8, &bw) != FR_OK || bw != 8)
        { f_close(&fil); return false; }

        // Occupied flags as uint8_t (bool size isn't guaranteed)
        uint8_t occ[PRESET_SLOTS];
        for(int s = 0; s < PRESET_SLOTS; s++) occ[s] = occupied_[s] ? 1 : 0;
        if(f_write(&fil, occ, PRESET_SLOTS, &bw) != FR_OK || bw != PRESET_SLOTS)
        { f_close(&fil); return false; }

        // All slot data (even unoccupied — simpler, and only 1 KB)
        if(f_write(&fil, data_, sizeof(data_), &bw) != FR_OK || bw != sizeof(data_))
        { f_close(&fil); return false; }

        f_close(&fil);
        return true;
    }

    bool LoadFromSD(FIL& fil, const char* path = "0:/presets.bin")
    {
        UINT br;
        if(f_open(&fil, path, FA_READ) != FR_OK) return false;

        // Verify magic
        uint8_t magic[8];
        if(f_read(&fil, magic, 8, &br) != FR_OK || br != 8)
        { f_close(&fil); return false; }
        static const uint8_t expected[8] = {'D','U','B','S','P','R','S','T'};
        if(memcmp(magic, expected, 8) != 0)
        { f_close(&fil); return false; }

        // Read occupied flags
        uint8_t occ[PRESET_SLOTS];
        if(f_read(&fil, occ, PRESET_SLOTS, &br) != FR_OK || br != PRESET_SLOTS)
        { f_close(&fil); return false; }
        for(int s = 0; s < PRESET_SLOTS; s++) occupied_[s] = (occ[s] != 0);

        // Read slot data
        if(f_read(&fil, data_, sizeof(data_), &br) != FR_OK || br != sizeof(data_))
        {
            // Partial read — wipe to be safe
            Init();
            f_close(&fil);
            return false;
        }

        f_close(&fil);
        return true;
    }

  private:
    float data_[PRESET_SLOTS][PRESET_PAGES][PRESET_KNOBS];
    bool  occupied_[PRESET_SLOTS];
};
} // namespace dubsauce

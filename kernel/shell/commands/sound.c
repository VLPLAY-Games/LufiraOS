#include "lib/types.h"
#include "drivers/console/console.h"
#include "drivers/sound/pcspeaker.h"
#include "drivers/sound/sound.h"
#include "../commands.h"

void command_beep(void)
{
    printf("\nPlaying 440 Hz beep...\n");

    pcspeaker_tone(440, 250);

    printf("Done.\n");
}

void command_mixer(const char* args)
{
    if (!args || *args == '\0') {
        printf("\nSound Mixer\n");
        printf("-----------\n");
        printf("Device : PC Speaker\n");
        printf("Volume : %u%%\n", sound_get_volume());
        printf("State  : %s\n",
               sound_is_available() ? "ready" : "offline");
        return;
    }

    uint32_t value = 0;

    while (*args >= '0' && *args <= '9') {
        value = value * 10 + (uint32_t)(*args - '0');
        args++;
    }

    if (value > 100) {
        printf("\nVolume must be between 0 and 100.\n");
        return;
    }

    sound_set_volume((uint8_t)value);

    printf("\nVolume set to %u%%\n", value);
}

static const sound_event_t test_melody[] = {
    { SOUND_NOTE_C4, 180, 30 },
    { SOUND_NOTE_D4, 180, 30 },
    { SOUND_NOTE_E4, 180, 30 },
    { SOUND_NOTE_C4, 180, 50 },

    { SOUND_NOTE_C4, 180, 30 },
    { SOUND_NOTE_D4, 180, 30 },
    { SOUND_NOTE_E4, 180, 30 },
    { SOUND_NOTE_C4, 250, 70 },

    { SOUND_NOTE_E4, 180, 30 },
    { SOUND_NOTE_F4, 180, 30 },
    { SOUND_NOTE_G4, 350, 80 }
};

void command_music(void)
{
    printf("\nPlaying test melody...\n");

    sound_play_melody(
        test_melody,
        sizeof(test_melody) / sizeof(test_melody[0])
    );

    printf("Done.\n");
}
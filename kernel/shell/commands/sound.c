#include "lib/types.h"
#include "drivers/console/console.h"
#include "drivers/sound/sound.h"
#include "../commands.h"


void command_beep(void)
{
    printf("\nPlaying 440 Hz beep...\n");

    if (!sound_is_available()) {
        printf("Error: audio is not available.\n");
        return;
    }

    if (sound_play_tone(440, 250) != 0) {
        printf("Error: failed to play tone.\n");
        return;
    }

    printf("Done.\n");
}


void command_mixer(const char* args)
{
    if (!args || *args == '\0') {
        printf("\nAudio Mixer\n");
        printf("-----------\n");

        if (!sound_is_available()) {
            printf("Device : %s\n", sound_driver_name());
            printf("State  : offline\n");
            return;
        }

        printf("Device : %s\n", sound_driver_name());
        printf("State  : ready\n");
        printf("Volume : %u%%\n", sound_get_volume());
        printf("Rate   : %u Hz\n", sound_get_sample_rate());

        return;
    }

    uint32_t value = 0;
    const char* p = args;

    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (uint32_t)(*p - '0');
        p++;
    }

    if (p == args) {
        printf("\nUsage: mixer [0-100]\n");
        return;
    }

    if (value > 100) {
        printf("\nVolume must be between 0 and 100.\n");
        return;
    }

    if (!sound_is_available()) {
        printf("\nError: audio is not available.\n");
        return;
    }

    sound_set_volume((uint8_t)value);

    printf("\nVolume set to %u%%\n", value);
}


typedef struct { uint32_t frequency, duration_ms; } sound_tone_t;

static const sound_tone_t test_melody[] = {
    { 262, 180 },   /* C4 */
    { 294, 180 },   /* D4 */
    { 330, 180 },   /* E4 */
    { 262, 180 },   /* C4 */

    { 262, 180 },   /* C4 */
    { 294, 180 },   /* D4 */
    { 330, 180 },   /* E4 */
    { 262, 250 },   /* C4 */

    { 330, 180 },   /* E4 */
    { 349, 180 },   /* F4 */
    { 392, 350 }    /* G4 */
};


void command_music(void)
{
    printf("\nPlaying test melody...\n");

    if (!sound_is_available()) {
        printf("Error: audio is not available.\n");
        return;
    }

    for (uint32_t i = 0;
         i < sizeof(test_melody) / sizeof(test_melody[0]);
         i++) {

        if (sound_play_tone(
                test_melody[i].frequency,
                test_melody[i].duration_ms) != 0) {

            printf("Error: failed at note %u.\n", i);
            return;
        }
    }

    printf("Done.\n");
}

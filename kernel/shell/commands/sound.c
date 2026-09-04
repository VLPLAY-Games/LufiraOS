#include "lib/types.h"
#include "drivers/console/console.h"
#include "drivers/sound/ac97.h"
#include "../commands.h"


void command_beep(void)
{
    printf("\nPlaying 440 Hz beep...\n");

    if (!ac97_is_available()) {
        printf("Error: AC'97 audio is not available.\n");
        return;
    }

    if (!ac97_play_tone(440, 250)) {
        printf("Error: failed to play tone.\n");
        return;
    }

    printf("Done.\n");
}


void command_mixer(const char* args)
{
    /*
     * Без аргументов — показать состояние аудиосистемы.
     */
    if (!args || *args == '\0') {
        printf("\nAudio Mixer\n");
        printf("-----------\n");

        if (!ac97_is_available()) {
            printf("Device : AC'97\n");
            printf("State  : offline\n");
            return;
        }

        printf("Device : AC'97\n");
        printf("State  : ready\n");
        printf("Volume : %u%%\n", ac97_get_volume());
        printf("Rate   : %u Hz\n", ac97_get_sample_rate());

        return;
    }

    /*
     * Читаем число 0..100.
     */
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

    if (!ac97_is_available()) {
        printf("\nError: AC'97 audio is not available.\n");
        return;
    }

    ac97_set_volume((uint8_t)value);

    printf("\nVolume set to %u%%\n", value);
}


static const ac97_tone_t test_melody[] = {
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

    if (!ac97_is_available()) {
        printf("Error: AC'97 audio is not available.\n");
        return;
    }

    for (uint32_t i = 0;
         i < sizeof(test_melody) / sizeof(test_melody[0]);
         i++) {

        if (!ac97_play_tone(
                test_melody[i].frequency,
                test_melody[i].duration_ms)) {

            printf("Error: failed at note %u.\n", i);
            return;
        }
    }

    printf("Done.\n");
}
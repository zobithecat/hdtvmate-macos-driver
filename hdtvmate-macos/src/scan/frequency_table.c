#include "channel_scan.h"
#include <stdio.h>

/*
 * US ATSC broadcast frequency table
 * UHF channels 14-51 (post-repack)
 * VHF channels 2-13
 *
 * Center frequencies in kHz
 */
static const atsc_frequency_t us_atsc_frequencies[] = {
    /* VHF Low Band (Channels 2-6) */
    { 2,  57000, "VHF-Lo"},
    { 3,  63000, "VHF-Lo"},
    { 4,  69000, "VHF-Lo"},
    { 5,  79000, "VHF-Lo"},
    { 6,  85000, "VHF-Lo"},

    /* VHF High Band (Channels 7-13) */
    { 7, 177000, "VHF-Hi"},
    { 8, 183000, "VHF-Hi"},
    { 9, 189000, "VHF-Hi"},
    {10, 195000, "VHF-Hi"},
    {11, 201000, "VHF-Hi"},
    {12, 207000, "VHF-Hi"},
    {13, 213000, "VHF-Hi"},

    /* UHF Band (Channels 14-36, post-repack active) */
    {14, 473000, "UHF"},
    {15, 479000, "UHF"},
    {16, 485000, "UHF"},
    {17, 491000, "UHF"},
    {18, 497000, "UHF"},
    {19, 503000, "UHF"},
    {20, 509000, "UHF"},
    {21, 515000, "UHF"},
    {22, 521000, "UHF"},
    {23, 527000, "UHF"},
    {24, 533000, "UHF"},
    {25, 539000, "UHF"},
    {26, 545000, "UHF"},
    {27, 551000, "UHF"},
    {28, 557000, "UHF"},
    {29, 563000, "UHF"},
    {30, 569000, "UHF"},
    {31, 575000, "UHF"},
    {32, 581000, "UHF"},
    {33, 587000, "UHF"},
    {34, 593000, "UHF"},
    {35, 599000, "UHF"},
    {36, 605000, "UHF"},

    /* Extended UHF (Channels 37-51, some still in use) */
    {38, 617000, "UHF"},
    {39, 623000, "UHF"},
    {40, 629000, "UHF"},
    {41, 635000, "UHF"},
    {42, 641000, "UHF"},
    {43, 647000, "UHF"},
    {44, 653000, "UHF"},
    {45, 659000, "UHF"},
    {46, 665000, "UHF"},
    {47, 671000, "UHF"},
    {48, 677000, "UHF"},
    {49, 683000, "UHF"},
    {50, 689000, "UHF"},
    {51, 695000, "UHF"},
};

static const uint32_t us_freq_count = sizeof(us_atsc_frequencies) / sizeof(us_atsc_frequencies[0]);

const atsc_frequency_t *frequency_table_get(uint32_t *count)
{
    *count = us_freq_count;
    return us_atsc_frequencies;
}

uint32_t frequency_for_channel(uint8_t channel_number)
{
    for (uint32_t i = 0; i < us_freq_count; i++) {
        if (us_atsc_frequencies[i].channel_number == channel_number) {
            return us_atsc_frequencies[i].frequency_khz;
        }
    }
    return 0;
}

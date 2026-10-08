#include "dd_media.h"

// Every URL checked on 2026-10-08, all answer with audio/mpeg.
// SomaFM refuses requests with no User-Agent, so the HTTP client must send one.
// archive.org answers each download with one redirect to an HTTPS mirror.
// Every stream here is MP3. The player picks its decoder from the URL's file
// extension and falls back to MP3 when there is none, so an extensionless AAC
// stream (RP Mellow at 128k is one) would be decoded as MP3 and fail.
// Speeches are public domain, US government works or LibriVox recordings.
const dd_media_t dd_media[] = {
    { DD_MEDIA_RADIO,  "Groove Salad",      "http://ice2.somafm.com/groovesalad-128-mp3" },
    { DD_MEDIA_RADIO,  "Drone Zone",        "http://ice2.somafm.com/dronezone-128-mp3" },
    { DD_MEDIA_RADIO,  "Deep Space One",    "http://ice2.somafm.com/deepspaceone-128-mp3" },
    { DD_MEDIA_RADIO,  "Ambient Sleeping Pill", "http://radio.stereoscenic.com/asp-h" },
    { DD_MEDIA_RADIO,  "Radio Paradise",    "http://stream.radioparadise.com/mp3-128" },
    { DD_MEDIA_RADIO,  "RP Mellow",         "http://stream.radioparadise.com/mellow-192" },
    { DD_MEDIA_RADIO,  "FIP Jazz",          "http://icecast.radiofrance.fr/fipjazz-midfi.mp3" },
    { DD_MEDIA_RADIO,  "Swiss Classic",     "http://stream.srg-ssr.ch/m/rsc_de/mp3_128" },
    { DD_MEDIA_RADIO,  "BBC World Service", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service" },

    { DD_MEDIA_SPEECH, "Kennedy, Inaugural 1961",
      "https://archive.org/download/JohnF.KennedyInauguralAddress/jfk_inaugural_address_64kb.mp3" },
    { DD_MEDIA_SPEECH, "Roosevelt, Fireside Chat 1941",
      "https://archive.org/download/FiresideChatWithFranklinDRoosevelt19411209FiresideChat/Fireside%20Chat%20with%20Franklin%20D%20Roosevelt%201941-12-09%20Fireside%20Chat.mp3" },
    { DD_MEDIA_SPEECH, "Lincoln, Gettysburg",
      "https://archive.org/download/gettysburg_address4_librivox/lincoln_gettysburg_address_64kb.mp3" },
    { DD_MEDIA_SPEECH, "Seneca, Shortness of Life",
      "https://archive.org/download/shortness_of_life_2305_librivox/shortness_01_seneca_64kb.mp3" },
    { DD_MEDIA_SPEECH, "Marcus Aurelius, Book 2",
      "https://archive.org/download/meditations_0708_librivox/meditations_02_marcusaurelius_64kb.mp3" },
};

const int dd_media_count = sizeof(dd_media) / sizeof(dd_media[0]);

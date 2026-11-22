// Far Cry 3: the game's language. The game picks its text (and menu) language from the system language
// (XGetLanguage), which the SDK answers from user_language. "auto" follows the Switch's system language (on the PC the
// Windows display language).
//
// This copy was written for Sonic Adventure 2; 2026-11-11 extended with Portuguese, Polish and Russian, and "auto"
// limited to the edition's languages (language_available). Far Cry 3 also has Portuguese (Brazil): the disc's common.dat carries the `pTBr`
// string, and its sound_<language>.dat files are the five European ones plus the Brazilian audio, all of them empty
// on this disc (every language's audio is inside sound.dat).
//
// TO CONFIRM AT THE FIRST BOOT, not to guess: what the game actually does with user_language, and whether it takes
// Portuguese from the disc's own data or needs one of the empty .dat files. Until then `brazilian` below is a guess
// on the id and must not be relied on: read the id out of the game's log once it runs, and fix the comment here.

#include "fc3_language.h"

#include <cctype>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(__SWITCH__)
#include <switch.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(language, "auto", "fc3",
                      "Game language: auto (the system language), english, japanese, french, german, spanish, "
                      "italian, portuguese, polish, russian");
// fc3-switch 2026-11-11: the languages this disc carries (text + voice). "auto" only picks one of these and falls back
// to English otherwise, so a console language the edition lacks never gets text or audio the disc does not have. The
// user's European disc is "(En,Pl,Ru)": sound_english / sound_russian .dat on the disc; Polish voice not checked.
REXCVAR_DEFINE_STRING(language_available, "english,polish,russian", "fc3",
                      "Languages of this edition that auto may pick (comma separated); others fall back to english");

namespace fc3::language {
namespace {

// Xbox 360 language ids (XLanguage, sdk include/rex/system/xcontent.h): 1 English, 2 Japanese, 3 German, 4 French,
// 5 Spanish, 6 Italian, 9 Portuguese, 11 Polish, 12 Russian. (The old Brazilian id 13 was out of range: XGetLanguage
// answered English for it.)
constexpr unsigned kEnglish = 1, kJapanese = 2, kGerman = 3, kFrench = 4, kSpanish = 5, kItalian = 6;
constexpr unsigned kPortugueseBR = 9, kPolish = 11, kRussian = 12;

const char* NameOf(unsigned id) {
  switch (id) {
    case kJapanese: return "japanese";
    case kGerman: return "german";
    case kFrench: return "french";
    case kSpanish: return "spanish";
    case kItalian: return "italian";
    case kPortugueseBR: return "portuguese";
    case kPolish: return "polish";
    case kRussian: return "russian";
    default: return "english";
  }
}

bool Available(unsigned id) {
  std::string list = REXCVAR_GET(language_available);
  for (char& c : list) c = char(std::tolower(static_cast<unsigned char>(c)));
  const std::string name = NameOf(id);
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    std::string item = list.substr(start, end - start);
    item.erase(0, item.find_first_not_of(" 	"));
    item.erase(item.find_last_not_of(" 	") + 1);
    if (item == name) return true;
    start = end + 1;
  }
  return false;
}

unsigned SystemLanguage() {
#if defined(__SWITCH__)
  unsigned id = kEnglish;
  if (R_SUCCEEDED(setInitialize())) {
    u64 code = 0;
    SetLanguage language = SetLanguage_ENUS;
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &language))) {
      switch (language) {
        case SetLanguage_JA: id = kJapanese; break;
        case SetLanguage_FR:
        case SetLanguage_FRCA: id = kFrench; break;
        case SetLanguage_DE: id = kGerman; break;
        case SetLanguage_IT: id = kItalian; break;
        case SetLanguage_ES:
        case SetLanguage_ES419: id = kSpanish; break;
        case SetLanguage_PT:
        case SetLanguage_PTBR: id = kPortugueseBR; break;
        case SetLanguage_RU: id = kRussian; break;
        default: id = kEnglish; break;
      }
    }
    setExit();
  }
  return id;
#elif defined(_WIN32)
  switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_JAPANESE: return kJapanese;
    case LANG_FRENCH: return kFrench;
    case LANG_GERMAN: return kGerman;
    case LANG_SPANISH: return kSpanish;
    case LANG_ITALIAN: return kItalian;
    case LANG_PORTUGUESE: return kPortugueseBR;
    case LANG_POLISH: return kPolish;
    case LANG_RUSSIAN: return kRussian;
    default: return kEnglish;
  }
#else
  return kEnglish;
#endif
}

}  // namespace

void Apply() {
  std::string name = REXCVAR_GET(language);
  for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  unsigned id = 0;
  if (name == "english") id = kEnglish;
  else if (name == "japanese") id = kJapanese;
  else if (name == "french") id = kFrench;
  else if (name == "german") id = kGerman;
  else if (name == "spanish") id = kSpanish;
  else if (name == "italian") id = kItalian;
  else if (name == "brazilian" || name == "portuguese") id = kPortugueseBR;
  else if (name == "polish") id = kPolish;
  else if (name == "russian") id = kRussian;
  else {
    if (name != "auto") REXLOG_WARN("language = \"{}\" is not known: auto is used", name);
    id = SystemLanguage();
    if (!Available(id)) {
      REXLOG_INFO("language: the system language ({}) is not on this edition: english", NameOf(id));
      id = kEnglish;
    }
  }
  rex::cvar::SetFlagByName("user_language", std::to_string(id));
  REXLOG_INFO("language: {} (Xbox language {})", name, id);
}

}  // namespace fc3::language

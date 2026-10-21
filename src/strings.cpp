// Find's own texts in the game's language (this file is UTF-8 with a byte order mark, so the
// u8"" literals stay UTF-8). The language is told by the game's own word for "Legendary"
// (FbUiLocaRpgTerms rarityLegendary), so it follows the language set in the game.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "log.h"
#include "strings.h"
#include "ui.h"

#define T(x) reinterpret_cast<const char*>(u8##x)

namespace strings {
namespace {

enum Lang { En, De, Fr, Es, It, Pt, Pl, Ru, Uk, Ja, Ko, Zh, Tr, LangCount };
const char* const kLangNames[LangCount] = {"en", "de", "fr", "es", "it", "pt", "pl", "ru", "uk", "ja", "ko", "zh", "tr"};
Lang g_lang = En;

// rarityLegendary: assets + 0x10 (FbUiLocaLinks) + 0x40 (rpgTerms) + 0x1dd0
constexpr size_t kLocaLegendary = 0x10 + 0x40 + 0x1dd0;

const char* const kTexts[LangCount][Count] = {
    // En
    {T("Find"), T("Type an item, a chest name, or a kind: category, rarity, perk."),
     T("Stand inside one of your bases to search its chests."), T("No item or chest name in this base matches \"%s\"."),
     T("Tab / Shift+Tab  choose     Click  choose     Enter  find"), T("Enter / Esc  close"), T("Category"), T("Rarity"),
     T("Perk"), T("Item type"), T("Chest"), T("Instruments"), T("Fishing Rods"), T("Animal Food"), T("Weapon Gems")},
    // De
    {T("Suchen"), T("Gegenstand, Truhenname oder Art eingeben: Kategorie, Seltenheit, Perk."),
     T("Stell dich in eine deiner Basen, um ihre Truhen zu durchsuchen."),
     T("Nichts in dieser Basis passt zu \"%s\"."), T("Tab / Umschalt+Tab  wählen     Klick  wählen     Enter  suchen"),
     T("Enter / Esc  schließen"), T("Kategorie"), T("Seltenheit"), T("Perk"), T("Gegenstandsart"), T("Truhe"),
     T("Instrumente"), T("Angeln"), T("Tierfutter"), T("Waffenedelsteine")},
    // Fr
    {T("Chercher"), T("Saisissez un objet, un nom de coffre ou un type : catégorie, rareté, atout."),
     T("Placez-vous dans l'une de vos bases pour fouiller ses coffres."),
     T("Rien dans cette base ne correspond à « %s »."), T("Tab / Maj+Tab  choisir     Clic  choisir     Entrée  chercher"),
     T("Entrée / Échap  fermer"), T("Catégorie"), T("Rareté"), T("Atout"), T("Type d'objet"), T("Coffre"),
     T("Instruments"), T("Cannes à pêche"), T("Nourriture animale"), T("Gemmes d'arme")},
    // Es
    {T("Buscar"), T("Escribe un objeto, el nombre de un cofre o un tipo: categoría, rareza, ventaja."),
     T("Colócate dentro de una de tus bases para buscar en sus cofres."),
     T("Nada en esta base coincide con \"%s\"."), T("Tab / Mayús+Tab  elegir     Clic  elegir     Intro  buscar"),
     T("Intro / Esc  cerrar"), T("Categoría"), T("Rareza"), T("Ventaja"), T("Tipo de objeto"), T("Cofre"),
     T("Instrumentos"), T("Cañas de pescar"), T("Comida para animales"), T("Gemas de arma")},
    // It
    {T("Cerca"), T("Scrivi un oggetto, il nome di una cassa o un tipo: categoria, rarità, vantaggio."),
     T("Mettiti dentro una delle tue basi per cercare nelle sue casse."),
     T("Niente in questa base corrisponde a \"%s\"."), T("Tab / Maiusc+Tab  scegli     Clic  scegli     Invio  cerca"),
     T("Invio / Esc  chiudi"), T("Categoria"), T("Rarità"), T("Vantaggio"), T("Tipo di oggetto"), T("Cassa"),
     T("Strumenti musicali"), T("Canne da pesca"), T("Cibo per animali"), T("Gemme per armi")},
    // Pt
    {T("Buscar"), T("Digite um item, o nome de um baú ou um tipo: categoria, raridade, vantagem."),
     T("Fique dentro de uma das suas bases para procurar nos baús dela."),
     T("Nada nesta base corresponde a \"%s\"."), T("Tab / Shift+Tab  escolher     Clique  escolher     Enter  buscar"),
     T("Enter / Esc  fechar"), T("Categoria"), T("Raridade"), T("Vantagem"), T("Tipo de item"), T("Baú"),
     T("Instrumentos"), T("Varas de pesca"), T("Comida de animais"), T("Gemas de arma")},
    // Pl
    {T("Szukaj"), T("Wpisz przedmiot, nazwę skrzyni lub rodzaj: kategoria, rzadkość, atut."),
     T("Stań w jednej ze swoich baz, aby przeszukać jej skrzynie."),
     T("Nic w tej bazie nie pasuje do „%s”."), T("Tab / Shift+Tab  wybierz     Klik  wybierz     Enter  szukaj"),
     T("Enter / Esc  zamknij"), T("Kategoria"), T("Rzadkość"), T("Atut"), T("Typ przedmiotu"), T("Skrzynia"),
     T("Instrumenty"), T("Wędki"), T("Karma dla zwierząt"), T("Klejnoty do broni")},
    // Ru
    {T("Найти"), T("Введите предмет, название сундука или вид: категория, редкость, перк."),
     T("Встаньте в одной из своих баз, чтобы искать в её сундуках."),
     T("В этой базе ничего не подходит под «%s»."), T("Tab / Shift+Tab  выбор     Клик  выбор     Enter  найти"),
     T("Enter / Esc  закрыть"), T("Категория"), T("Редкость"), T("Перк"), T("Тип предмета"), T("Сундук"),
     T("Инструменты"), T("Удочки"), T("Корм для животных"), T("Самоцветы для оружия")},
    // Uk
    {T("Знайти"), T("Введіть предмет, назву скрині або вид: категорія, рідкісність, перк."),
     T("Станьте в одній зі своїх баз, щоб шукати в її скринях."),
     T("У цій базі нічого не відповідає «%s»."), T("Tab / Shift+Tab  вибір     Клік  вибір     Enter  знайти"),
     T("Enter / Esc  закрити"), T("Категорія"), T("Рідкісність"), T("Перк"), T("Тип предмета"), T("Скриня"),
     T("Інструменти"), T("Вудки"), T("Корм для тварин"), T("Самоцвіти для зброї")},
    // Ja
    {T("検索"), T("アイテム名、チェスト名、または種類（カテゴリー・レア度・パーク）を入力してください。"),
     T("拠点の中に立つと、その拠点のチェストを検索できます。"), T("この拠点には「%s」に一致するものがありません。"),
     T("Tab / Shift+Tab  選択     クリック  選択     Enter  検索"), T("Enter / Esc  閉じる"), T("カテゴリー"),
     T("レア度"), T("パーク"), T("アイテムの種類"), T("チェスト"), T("楽器"), T("釣り竿"), T("動物のエサ"),
     T("武器用ジェム")},
    // Ko
    {T("찾기"), T("아이템, 상자 이름 또는 종류(카테고리, 등급, 특성)를 입력하세요."),
     T("자신의 기지 안에 서야 그 기지의 상자를 검색할 수 있습니다."), T("이 기지에는 \"%s\"와(과) 일치하는 것이 없습니다."),
     T("Tab / Shift+Tab  선택     클릭  선택     Enter  찾기"), T("Enter / Esc  닫기"), T("카테고리"), T("등급"),
     T("특성"), T("아이템 종류"), T("상자"), T("악기"), T("낚싯대"), T("동물 먹이"), T("무기 보석")},
    // Zh
    {T("查找"), T("输入物品、箱子名称或类别：分类、稀有度、特性。"), T("站在你的某个基地内才能搜索其箱子。"),
     T("此基地中没有与“%s”匹配的内容。"), T("Tab / Shift+Tab  选择     点击  选择     Enter  查找"), T("Enter / Esc  关闭"),
     T("分类"), T("稀有度"), T("特性"), T("物品类型"), T("箱子"), T("乐器"), T("钓竿"), T("动物饲料"), T("武器宝石")},
    // Tr
    {T("Bul"), T("Bir eşya, sandık adı ya da tür yazın: kategori, nadirlik, yetenek."),
     T("Sandıklarını aramak için üslerinden birinin içinde dur."), T("Bu üste \"%s\" ile eşleşen bir şey yok."),
     T("Tab / Shift+Tab  seç     Tıkla  seç     Enter  bul"), T("Enter / Esc  kapat"), T("Kategori"), T("Nadirlik"),
     T("Yetenek"), T("Eşya türü"), T("Sandık"), T("Enstrümanlar"), T("Oltalar"), T("Hayvan yemi"), T("Silah taşları")},
};

// Plural forms: 0 one, 1 few, 2 many (en-like languages use 0 and 2).
int PluralForm(uint32_t n)
{
    switch (g_lang) {
    case Ja: case Ko: case Zh: case Tr:
        return 2;
    case Fr:
        return n <= 1 ? 0 : 2;
    case Ru: case Uk:
        if (n % 10 == 1 && n % 100 != 11)
            return 0;
        return n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14) ? 1 : 2;
    case Pl:
        if (n == 1)
            return 0;
        return n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14) ? 1 : 2;
    default:
        return n == 1 ? 0 : 2;
    }
}

// "in N chest(s)", by plural form (one, few, many).
const char* const kInChests[LangCount][3] = {
    {T("in %d chest"), T("in %d chests"), T("in %d chests")},
    {T("in %d Truhe"), T("in %d Truhen"), T("in %d Truhen")},
    {T("dans %d coffre"), T("dans %d coffres"), T("dans %d coffres")},
    {T("en %d cofre"), T("en %d cofres"), T("en %d cofres")},
    {T("in %d cassa"), T("in %d casse"), T("in %d casse")},
    {T("em %d baú"), T("em %d baús"), T("em %d baús")},
    {T("w %d skrzyni"), T("w %d skrzyniach"), T("w %d skrzyniach")},
    {T("в %d сундуке"), T("в %d сундуках"), T("в %d сундуках")},
    {T("у %d скрині"), T("у %d скринях"), T("у %d скринях")},
    {T("チェスト%d個"), T("チェスト%d個"), T("チェスト%d個")},
    {T("상자 %d개"), T("상자 %d개"), T("상자 %d개")},
    {T("%d 个箱子"), T("%d 个箱子"), T("%d 个箱子")},
    {T("%d sandıkta"), T("%d sandıkta"), T("%d sandıkta")},
};

// "N item(s)" (different items of a kind), by plural form.
const char* const kItems[LangCount][3] = {
    {T("%u item"), T("%u items"), T("%u items")},
    {T("%u Gegenstand"), T("%u Gegenstände"), T("%u Gegenstände")},
    {T("%u objet"), T("%u objets"), T("%u objets")},
    {T("%u objeto"), T("%u objetos"), T("%u objetos")},
    {T("%u oggetto"), T("%u oggetti"), T("%u oggetti")},
    {T("%u item"), T("%u itens"), T("%u itens")},
    {T("%u przedmiot"), T("%u przedmioty"), T("%u przedmiotów")},
    {T("%u предмет"), T("%u предмета"), T("%u предметов")},
    {T("%u предмет"), T("%u предмети"), T("%u предметів")},
    {T("%u種類"), T("%u種類"), T("%u種類")},
    {T("%u종"), T("%u종"), T("%u종")},
    {T("%u 种"), T("%u 种"), T("%u 种")},
    {T("%u eşya"), T("%u eşya"), T("%u eşya")},
};

bool StartsWith(const char* s, size_t len, const char* prefix)
{
    size_t n = strlen(prefix);
    if (len < n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

bool Contains(const char* s, size_t len, const char* part)
{
    size_t n = strlen(part);
    for (size_t i = 0; i + n <= len; ++i)
        if (memcmp(s + i, part, n) == 0)
            return true;
    return false;
}

} // namespace

const char* Get(Id id)
{
    return id >= 0 && id < Count ? kTexts[g_lang][id] : "";
}

std::string ItemCount(uint32_t amount, int chests)
{
    char where[96], buf[160];
    _snprintf_s(where, _TRUNCATE, kInChests[g_lang][PluralForm(static_cast<uint32_t>(chests))], chests);
    if (g_lang == Ja || g_lang == Ko || g_lang == Zh)
        _snprintf_s(buf, _TRUNCATE, "%u\xEF\xBC\x88%s\xEF\xBC\x89", amount, where); // 996（チェスト1個）
    else if (g_lang == Tr)
        _snprintf_s(buf, _TRUNCATE, "%u, %s", amount, where);
    else
        _snprintf_s(buf, _TRUNCATE, "%u %s", amount, where);
    return buf;
}

std::string KindCount(uint32_t items, int chests)
{
    char what[96], where[96], buf[200];
    _snprintf_s(what, _TRUNCATE, kItems[g_lang][PluralForm(items)], items);
    _snprintf_s(where, _TRUNCATE, kInChests[g_lang][PluralForm(static_cast<uint32_t>(chests))], chests);
    if (g_lang == Ja || g_lang == Ko || g_lang == Zh || g_lang == Tr)
        _snprintf_s(buf, _TRUNCATE, "%s, %s", what, where);
    else
        _snprintf_s(buf, _TRUNCATE, "%s %s", what, where);
    return buf;
}

void Detect()
{
    ui::Text t = ui::Loca(kLocaLegendary);
    if (!t.utf8 || !t.len)
        return;
    const char* s = t.utf8;
    size_t n = t.len;
    Lang lang = En;
    if (StartsWith(s, n, "legendary"))
        lang = En;
    else if (StartsWith(s, n, T("legendär")))
        lang = De;
    else if (StartsWith(s, n, T("légend")) || StartsWith(s, n, T("lÉgend")))
        lang = Fr;
    else if (StartsWith(s, n, "legendari"))
        lang = Es;
    else if (StartsWith(s, n, "leggend"))
        lang = It;
    else if (StartsWith(s, n, "lend"))
        lang = Pt;
    else if (StartsWith(s, n, "legendarn"))
        lang = Pl;
    else if (StartsWith(s, n, "efsane"))
        lang = Tr;
    else if (Contains(s, n, T("ы")) || Contains(s, n, T("Ы")))
        lang = Ru;
    else if (Contains(s, n, T("Леген")) || Contains(s, n, T("ЛЕГЕН")))
        lang = Uk;
    else if (Contains(s, n, T("전설")))
        lang = Ko;
    else if (Contains(s, n, T("レジェ")) || Contains(s, n, T("伝説")))
        lang = Ja;
    else if (Contains(s, n, T("传")) || Contains(s, n, T("傳")))
        lang = Zh;
    if (lang != g_lang)
        Log("language: %s (the game's \"Legendary\": %.*s)", kLangNames[lang], static_cast<int>(n), s);
    g_lang = lang;
}

const char* Language()
{
    return kLangNames[g_lang];
}

} // namespace strings

#!/usr/bin/env python3
# tools/aurelith_books.py
#
# The text of Aurelith's lore books and signs (docs/hush-lore.md is the canon
# they follow). Pure data: tools/gen_aurelith.py imports BOOKS, BOOK_HOMES and
# SIGNS and writes them into the city's loot tables
# (data/minecraft/loot_table/chests/aurelith_*.json, as minecraft:written_book
# with set_book_cover + set_written_book_pages) and into its structure
# templates (lecterns carry a written book, signs carry front_text). Change
# the words here, never in the generated JSON or NBT.
#
#   python3 tools/aurelith_books.py     # self-check: titles, page budgets, signs
#
# Page budget: MC's book page is 14 lines of roughly 19 characters in the
# default font (BookViewScreen: 114 px wide text area). Every page must be
# <= 220 characters and wrap to <= 14 lines at 19 characters, counting every
# explicit "\n" (and every blank line) as a line. Sign lines are <= 15
# characters (MC's sign is 90 px wide). Plain text only: no JSON components,
# no formatting codes.

from __future__ import annotations

import sys

BOOKS: dict[str, dict] = {
    # ── The Charter of Aurelith ─────────────────────────────────────────────
    "charter": {
        "title": "The Charter of Aurelith",
        "author": "The First Conductor",
        "pages": [
            "THE CHARTER\nOF AURELITH\n\nSet down in the\nfirst year of the\nChord, over the\nspring that sings.",
            "We came through the\ngates into a world\nwith no sound in it.\n\nHere a note does\nnot fade. Here we\nwill build a city\nthat never has to\nstop singing.",
            "I. The Heart shall\nhold the Chord.\nWhile it holds, the\nlanterns burn, the\ncrystal grows and\nthe gates stay open.",
            "II. Every voice is\na lantern. No one\nin Aurelith sings\nalone, and no one\nis made to be\nsilent.",
            "III. Listen first.\nSpeak second. In\nthe Plaza of the\nHeld Note, speak\nnot at all.",
            "IV. Four gates for\nfour voices:\nSoprano to the\nnorth, Alto to the\neast, Tenor to the\nsouth, Bass to the\nwest. Their lights\nshall stand so no\ntraveller is lost.",
            "V. The river Vesper\nis the Chord's own\nwater. Drink of it,\nsail on it, and\nnever wall it in.",
            "VI. The Wardens\nguard the gates and\nthe Heart. They\nanswer to the\nconductor alone.",
            "VII. Should the\nChord ever fail,\nthe conductor alone\nmay call a Rest.\n\nA rest is not an\nending. It is the\nsilence the next\nnote needs.",
            "Sung and set in\nstone, C. 1.\n\nWHAT IS SUNG IS\nNEVER LOST.",
        ],
    },
    # ── Log of the Heart ────────────────────────────────────────────────────
    "heart_log": {
        "title": "Log of the Heart",
        "author": "Orrin of the Tuners",
        "pages": [
            "LOG OF THE HEART\nkept at the Tuners'\nWorks by Orrin,\nMaster Tuner.\n\nBook the ninth.\nC. 1139 onward.",
            "C.1139, 3rd rest.\nOuter ring drift\n0.2 of a comma.\nRetuned. Conduit\nseven cloudy,\nreplaced. Chord\nsteady. Tea cold.\nAs usual.",
            "C.1140. Up the mast\non our roof to set\nthe lighthouses'\nnote against the\nTenor. Left the\nTenor's key up\nthere again. It is\nsafest in the wind.",
            "C.1141. Drift 0.5.\nRetuned twice.\nApprentice swears\nthe inner ring\nturned backwards\nfor a breath. It\ndid not. Rings do\nnot turn backwards.",
            "C.1141. Full\nretune. Voices set\nas always, floor\nto crown: the Bass\nfirst, the Tenor,\nthe Alto, and the\nSoprano to close\nit. Apprentice\nstill asks why.",
            "C.1142. Drift 1.1.\nThere is a second\nnote under ours.\nVery low. Below\nhearing, but the\nwater in my cup\nhears it. Liesl\nsays she hears it\ntoo. Log it and\nmove on.",
            "C.1144. Lanterns\nin the Stillhouses\nflickered for the\nlength of a breath.\nThe whole city.\nNot us. Not the\nHeart. I checked\nevery conduit\nmyself.",
            "C.1145. Drift 3.\nRetuning makes it\nworse. It is not\ndrift. Something\nis singing back,\nand it is learning\nthe Chord.",
            "C.1146. Stopped\nsleeping in the\nWorks. The rings\nhum in my teeth.\nThe crystal on the\nnorth conduit grew\na finger's width\novernight. Dark.",
            "C.1147. Ysolde came\nto the Works\ntonight. She asked\nhow long the Heart\ncould be held.\nI told her the\ntruth. Not long.",
            "C.1147. Rest is\ncalled for the\nfourth bell.\nConduits drained.\nRings locked.\nThey still turn.\nI do not know why\nthey still turn.",
            "Last entry.\nIt stopped.\nIt is so quiet.\nI can hear my own\nheart and it is\nthe loudest thing\nin the city.\n\nOrrin.",
        ],
    },
    # ── The Conductor's Diary ───────────────────────────────────────────────
    "diary": {
        "title": "The Conductor's Diary",
        "author": "Ysolde",
        "pages": [
            "I am the last to\nkeep this book.\nNine conductors\nwrote in it before\nme. None of them\nwrote about the\nend. I will have\nto.",
            "C.1146.\nThe children dream\nthe same dream.\nA deep place, and\na voice in it that\nsounds like ours\nbut slower. Liesl\ncalls it the\nUndersong.",
            "Stood on the Podium\nfacing the Heart\ntonight. Every\nring was humming a\nhalf-tone flat. I\nsang against it and\nit bent back, for a\nmoment. Only for a\nmoment.",
            "Orrin says the\nChord is being\nlearned. If it\nlearns the whole\nChord it will not\nneed us to hold it.\nI do not want to\nknow what it will\nsing then.",
            "C.1147.\nThe Tuners' vote\nis unanimous. The\nArchive's too. The\nchoice is still\nmine. That is the\none law we never\nwrote down.",
            "We will Rest.\nThe Heart goes\nsilent at the\nfourth bell. The\ncity walks out\nthrough the gates\nin one night: the\nLast Procession.",
            "The Wardens will\nstay. They cannot\nbe told the song is\nover. They were\nmade to hear a\nfalse note, and now\nevery note will be\nfalse.",
            "I have sealed the\nlast of our best\nthings under the\ndais. The Bass\nholds it. The deep\nvoice always did\nthe holding.",
            "The four keys are\nscattered: the deep\none below, the high\none with Liesl, the\nothers with their\nguilds. Whoever\ngathers them must\nnot sing them.",
            "Someone has to\nstand on the Podium\nwhen the Heart\nstops, to be sure\nit stays stopped.\nIt should be the\nconductor.\n\nIt will be me.",
        ],
    },
    # ── A Lesson for Small Voices ───────────────────────────────────────────
    "lesson": {
        "title": "A Lesson for Small Voices",
        "author": "Aubade",
        "pages": [
            "MY LESSON BOOK\nAubade, age 8\nStillhouse Row\n\nDo not draw in\nthis book.\n(I did not.)",
            "THE FOUR VOICES\nSoprano is high.\nAlto is warm.\nTenor is bright.\nBass is deep and\nholds everyone up.\n\nI am a Soprano.\nThe teacher says\nnot yet.",
            "WHY WE WHISPER\nThe Hush has no\nnoise of its own so\nall noise is ours.\nIf you shout, the\nwhole world has\nto hear it.\n\nGood. -T.",
            "WHY LANTERNS GLOW\nLight is song you\ncan see. A lantern\nis a little bit of\nthe Chord in a\ncage so it does\nnot fly off.\n\n(It does not fly.\nIt fades. -T.)",
            "THE HEART\nThe Heart is the\nbiggest instrument\nthere is. It sings\none note forever\nand the rings go\nround and round.\nI am not allowed\nto touch the rings.",
            "THE WARDENS\nWardens have no\neyes because they\nnever needed them.\nThey will not hurt\nyou if you are\nquiet.\n\nVery quiet. -T.",
            "HOW TO SING THE\nCHORD\nStart at the bottom.\nYou cannot put a\nroof on nothing.\nSo the deep one\nfirst and the high\none LAST.\n\nThat is me. -A.",
            "My dream last night\nwas the deep place\nagain. Everyone in\nclass has it. The\nvoice was singing\nour Chord but\nwrong, like it was\nstill learning.",
            "Teacher says we\nare going on a walk\ntonight, the whole\ncity, and I can\nbring one thing.\n\nI am bringing this\nbook.",
            "(page torn out)",
        ],
    },
    # ── To Whoever Comes After ──────────────────────────────────────────────
    "broadcast": {
        "title": "To Whoever Comes After",
        "author": "Ysolde",
        "pages": [
            "To whoever comes\nafter.\n\nIf you are reading\nthis, a gate has\nopened that we\nshut, and you have\nwalked into our\ncity. Welcome.",
            "You will think it\nempty. It is not.\nListen. The lamps\nstill burn because\ncrystal holds light\nthe way a lung\nholds breath, and\nlets it go slowly.",
            "Aurelith is still\nsinging. Very\nquietly. Too\nquietly for the\nthing below to\nlearn from it,\nI hope.",
            "Do not wake the\nHeart. Whatever\nyou are, whatever\nyou can do. The\nChord was not\nours alone at the\nend.",
            "The Wardens are\nnot cruel. They\nare still on duty.\nWalk softly past\nthem, as we did.",
            "You may meet me.\nI will not be as I\nwas. The silence\nwe left grows, and\nI stood in the\nmiddle of it.\n\nIf I am singing,\ndo not answer.",
            "Under the Heart is\nwhat we could not\ncarry. Take it. We\nwould rather it\nwent on than lay\nin the dark.\n\nThe deep voice\nholds the way down.",
            "What is sung is\nnever lost.\n\nRest is not an\nending.\n\n- Ysolde,\nlast conductor of\nAurelith. C. 1147.",
        ],
    },
    # ── A Legend of Aurelith ────────────────────────────────────────────────
    "legend": {
        "title": "A Legend of Aurelith",
        "author": "Corvin of the Quays",
        "pages": [
            "A LEGEND OF\nAURELITH\nfor travellers who\narrive by gate or\nby water.\n\nCorvin, harbour-\nmaster, Vesper\nQuays. C. 1102.",
            "THE GATES\nFour towers, four\nbeams. From the\nplain, walk toward\nthe light: every\nbeam stands on a\ngate. Soprano N,\nAlto E, Tenor S,\nBass W.",
            "THE PLAZA OF THE\nHELD NOTE\nAt the centre. The\nHeart hangs above\nits dais. Canticle\nWay runs north and\nsouth from here,\nHeld Note Avenue\neast and west.",
            "Mind the Podium,\nfacing the Heart.\nIt is the\nconductor's. Do not\nstand on it. People\ndo. The Wardens\nhave views.",
            "THE VESPER QUAYS\nFollow the glow.\nThe river crosses\nthe city under\nthree arched\nbridges. Moor at\nthe stone posts,\nnot the lamps.",
            "THE ARCADE OF\nECHOES\nThe long market\nunder the columns.\nEverything is paid\nfor in echoes.\nHesper's lamps are\nfair; others less.",
            "THE STILLHOUSES\nThe quiet quarter.\nTerraces, dark\nglass, amber eaves.\nIf a door is shut,\nthey are sleeping.\nIf it is open,\nknock anyway.",
            "THE ARCHIVE OF\nECHOES\nThe tower of\nshelves. Listen\nfirst; the\nListeners will\nnot let you in\notherwise.",
            "STARWARD\nThe domed tower.\nClimb it on a\nstrong aurora.\n\nTHE QUIET GARDENS\nStepped terraces.\nNo talking. Truly.",
            "THE CONDUCTOR'S\nSPIRE\nThe tallest tower.\nFrom its top\nterrace the four\nbeams, the river\nand the Heart all\nline up. Go once.\nEveryone should.",
        ],
    },
    # ── Canticle of the Vesper ──────────────────────────────────────────────
    "canticle": {
        "title": "Canticle of the Vesper",
        "author": "Unknown",
        "pages": [
            "CANTICLE OF\nTHE VESPER\n\nsung at the Quays\nat the lamp-hour",
            "Vesper, slow water,\nviolet under\ncyan,\nyou carry the note\nwe cannot hold,\ndown through the\ncity and on.",
            "Fishers say your\ndepth is light.\nChildren say your\nlight is deep.\nBoth drink from\nyou; neither\nsleeps the worse.",
            "When the Heart\nsings, you shine.\nWhen the Heart\nrests, you will\nshine a while,\nthe way a bell\nkeeps ringing\nafter the hand\nlets go.",
            "Vesper, remember\nus. You were here\nbefore the first\nChord. You will be\nhere after the\nlast.",
            "What is sung\ninto water\nis never lost.\nIt only goes\ndownstream.",
        ],
    },
    # ── The Lampwright's Primer ─────────────────────────────────────────────
    "primer": {
        "title": "The Lampwright's Primer",
        "author": "Hesper of the Lampwrights",
        "pages": [
            "THE LAMPWRIGHT'S\nPRIMER\n\nfor apprentices\nwho have already\nbroken one lantern\nand would like to\nstop.",
            "I. Light is song\nmade visible.\nA lantern does not\nmake light. It\nholds a note, and\nthe note shows.",
            "II. Grow the\ncrystal under a\nsteady voice. A\nwavering voice\ngrows a cloudy\ncrystal. A cloudy\ncrystal flickers.\nCustomers notice.",
            "III. Cage it in\nresonite, never\niron. Iron is\ndeaf. Resonite\nlistens with the\ncrystal and keeps\nit company.",
            "IV. Panels: set\nsong-light in glass\nfor walls. Cyan is\nthe Chord. Violet\nis the river.\nAmber is a hearth.\nDo not mix amber\ninto the Plaza.",
            "V. A lantern is\ntuned, not lit.\nHum its note to\nit. If it brightens\nyou have the right\nnote. If it goes\nout, you have\nlearned something.",
            "VI. A lamp far\nfrom the Heart\nwill fade in a\nhundred years.\nNear the Heart,\nnever.\n\nAll the more reason\nto live close in.",
        ],
    },
    # ── On the Making of Wardens ────────────────────────────────────────────
    "wardens": {
        "title": "On the Making of Wardens",
        "author": "The Keeper",
        "pages": [
            "ON THE MAKING\nOF WARDENS\n\nFor the Keepers\nwho come after me.\nRead once. Then\nlock it away.",
            "A Warden is not\nbuilt. It is grown\nin the song, deep,\nwhere the Chord is\nloudest. It takes\nforty years. Most\ndo not wake.",
            "We grow them\nwithout eyes. A\nguardian that\nlooks can be\nfooled. One that\nlistens cannot.",
            "They are taught\none thing: the\nChord is right and\neverything else is\na false note.\nFind the false note\nand silence it.",
            "They are gentle\nwith the quiet.\nA sleeping child\ncan lie against a\nWarden's foot and\nbe safe as stone.",
            "I have wondered\nwhat a Warden would\ndo if the Chord\never stopped. The\nanswer frightens\nme too much to\nwrite down.",
            "So I will only\nwrite this: never\nstop the Chord\nwhile a Warden\nstill stands.\n\nThey cannot be\ntold it is over.",
        ],
    },
    # ── The Four Voices ─────────────────────────────────────────────────────
    "four_voices": {
        "title": "The Four Voices",
        "author": "Liesl of the Listeners",
        "pages": [
            "THE FOUR VOICES\n\nThe Chord is four\nparts sung as one.\nAurelith has a\ngate for each, and\neach gate a light\nthat stands into\nthe sky.",
            "SOPRANO, north.\nThe high voice\nkeeps watch. Its\nbeam is the palest.\nThe Last\nProcession left\nthe city by it.",
            "ALTO, east.\nThe warm voice\nwelcomes. Most\ntravellers come in\nunder its beam, and\nthe Arcade opens\nto meet them.",
            "TENOR, south.\nThe bright voice\ncarries. Its beam\nreaches furthest;\nthe lighthouses on\nthe plains were\ntuned from it.",
            "BASS, west.\nThe deep voice\nholds. Its beam is\nthe darkest and the\nsteadiest. In every\nchord the bass is\nthe floor the\nothers stand on.",
            "Four statues face\nthe Heart, one for\neach voice. Their\nfaces are blank so\nanyone may sing in\nthem.",
            "A chord is raised\nas a house is: the\nfloor first, the\nroof last. Deep,\nbright, warm, high.\nSung any other way\nit falls.",
            "The Alto keeps the\nlamplighters' song:\nthe hearth, the\nriver, the Chord,\nthe hearth again.\nThe Hall's cabinet\nwas tuned to it.",
            "The fourth voice\npoints down.\nIt always has.",
        ],
    },
    # ── Ledger of the Arcade ────────────────────────────────────────────────
    "ledger": {
        "title": "Ledger of the Arcade",
        "author": "Hesper of the Lampwrights",
        "pages": [
            "LEDGER\nHesper's Lamps,\nstall nine,\nArcade of Echoes.\n\nPrices in echoes\n(shards).",
            "C.1146\nEcho lantern, std.\n2\nSame, hanging\n2 + chain\nCyan panel x4\n1\nViolet panel x4\n1\nAmber panel x4\n1 (Stillhouse\nrate)",
            "Retuning, any lamp\n1\nRetuning, lamp\nsomeone else\nbroke\n3\nRetuning, lamp my\napprentice broke\nno charge, and a\nstern word",
            "Owed:\nCorvin, 3 mooring\nlamps. Paid in\nfish. Again.\nOrrin, 12 conduit\ncrystals. Tuners\nnever pay.\nAubade's mother,\n1 nightlight. Gift.",
            "C.1147\nLamps returned for\nflicker: 14\nLamps returned for\nflicker, whole\nstreet: 40\nNot the lamps.\nNot my fault.",
            "Closing the stall\nfor the Procession.\nTook the till.\nLeft the stock.\n\nIf you need a lamp,\ntake one. Pay me\nback when we\ncome home.",
        ],
    },
    # ── Starward Observations ───────────────────────────────────────────────
    "starward": {
        "title": "Starward Observations",
        "author": "Tamsin of the Starward",
        "pages": [
            "STARWARD\nOBSERVATIONS\nof the aurora\nand the cyan stars\n\nTamsin, observer",
            "The Hush has no\nsun and no moon.\nIts stars do not\nmove. Only the\naurora moves, so\nthe aurora is what\nwe watch.",
            "Thesis: the aurora\nis the Chord's\nafterimage. The\nHeart sings; the\nsky remembers it\nin colour.",
            "Evidence: it runs\ncyan to violet,\nthe same as the\nlanterns and the\nriver. It is\nbrightest over the\nsteppe, where the\nChoir Hall stands.",
            "C.1145. The aurora\nflickered in time\nwith the Stillhouse\nlamps. A beat\nlater. Like an\necho of an echo.",
            "C.1147, after the\nRest. The aurora\ndid not go out.\nFainter, but there.\n\nThe sky is still\nremembering us.",
            "If that is true\nthen nothing we\nsang is gone.\n\nI will take that\nwith me through\nthe gate.",
        ],
    },
    # ── A Letter, Unsent ────────────────────────────────────────────────────
    "letter": {
        "title": "A Letter, Unsent",
        "author": "Maren",
        "pages": [
            "Dear Tobin,\n\nYou will have gone\nahead with the\nTenor families by\nthe time this\nreaches you. If it\nreaches you.",
            "They told us to\nbring one thing\neach. I stood in\nour kitchen for an\nhour. The table is\nstill laid. I could\nnot clear it. It\nfelt like a lie.",
            "The whole street\nis walking north to\nthe Soprano Gate.\nNobody is talking.\nYou would laugh.\nThe quietest city\nin any world, and\nit got quieter.",
            "I left the lamp in\nthe window on, so\nthe house would not\nbe dark. Hesper\nsays it will burn\nfor a hundred\nyears. Good.",
            "I wrote on the\nwall by the gate,\nwhere they will\nsee it, that we\nwill come back.",
            "I do not know if\nthat is true. I\nwanted it to be\nthe last thing\nsomeone read here.",
            "Keep singing where\nyou are. Softly.\n\nYour Maren",
        ],
    },
    # ── The Undersong ───────────────────────────────────────────────────────
    "undersong": {
        "title": "The Undersong",
        "author": "Liesl of the Listeners",
        "pages": [
            "THE UNDERSONG\n\nwhat was heard\nbeneath the Chord,\nC. 1142 to 1147.\n\nNot to be sung.\nNot to be read\naloud.",
            "It began below\nhearing. We found\nit in the water\nfirst: rings on a\nstill cup, in time\nwith nothing we\nwere playing.",
            "We caught it in\nshards and slowed\nit. It is a voice.\nIt is very large\nand very far down,\nand it is singing\nthe Chord.",
            "Not copying.\nLearning. Each year\nit sang more of it.\nIn C.1145 it sang\nthe Bass part\nwhole. Better than\nwe do.",
            "The Soprano's key\nis here with me in\nthe cell. The high\nvoice keeps watch\nover what must not\nbe sung.",
            "I have sealed the\nshards in the\nArchive's lowest\nshelf. Do not play\nthem. When you\nplay them, it\nlistens back.",
            "The Heart will be\nsilenced tonight.\nYsolde believes a\nsong that stops\ncannot be learned.\nI want to believe\nher.",
            "But I played the\nlast shard again\nthis evening, and\nthere was a pause\nin it, where our\nChord ends, and\nthen it",
        ],
    },
    # ── Coda ─────────────────────────────────────────────────────────────────
    # Revealed, not found: the server sets it open on the Conductor's seat when
    # the Unsung falls and the Chord resolves (server/level/AurelithCities,
    # RevealCoda). The engine reads it from GeneratedAurelithBooks.inc
    # (python3 tools/aurelith_books.py --emit-cpp).
    "coda": {
        "title": "Coda",
        "author": "Ysolde",
        "pages": [
            "CODA\n\nfor whoever sings\nthe Heart awake.\n\nIf you read this,\nthe Chord is held\nagain, and you are\nstill here.",
            "Then you heard\nwhat answered it.\nI heard it too, on\nthe night of the\nRest. It had\nlearned our Chord\nso well it did not\nneed us to sing it.",
            "So I stopped the\nsong. A thing that\nlearns by listening\ncannot learn a\nsilence. That was\nall the Rest ever\nwas: the one note\nit could not copy.",
            "You did what I\ncould not. You sang\nit back, and kept\nyour own voice.\n\nHold the note. A\nlittle of it is\nyours now.",
            "If you find the\nones who left, tell\nthem the lamps are\nlit. Tell them the\nwall still has\ntheir words on it.\n\nWe will come back.",
            "It was never going\nto be me who came\nback. I knew that\nwhen I stayed.\n\nIt was always\ngoing to be you.\n\n- Ysolde",
        ],
    },
}

# Where each book lives: the city districts' chests (chests/aurelith_<home>)
# and lecterns draw from these lists. Every key is a BOOKS key.
BOOK_HOMES: dict[str, list[str]] = {
    "plaza":       ["charter", "four_voices"],
    "archive":     ["undersong", "wardens", "charter", "four_voices"],
    "stillhouses": ["lesson", "letter"],
    "arcade":      ["ledger", "primer"],
    "quays":       ["canticle", "legend"],
    "gates":       ["legend", "four_voices"],
    "spire":       ["diary", "legend"],
    "observatory": ["starward"],
    "tuners":      ["heart_log", "primer"],
    "vault":       ["broadcast", "diary", "undersong"],
    "gardens":     ["canticle", "starward"],
    "instruments": ["four_voices", "charter"],
    # Not a chest's: the Coda is set on the Conductor's seat by the server
    # when the city is awakened (never rolled as loot).
    "revealed":    ["coda"],
}

# Signs: four lines of <= 15 characters (street names, plaques, mottos).
SIGNS: dict[str, list[str]] = {
    "canticle_way":      ["Canticle Way", "", "", ""],
    "held_note_avenue":  ["Held Note", "Avenue", "", ""],
    "lantern_walk":      ["Lantern Walk", "", "", ""],
    "vesper_quay":       ["Vesper Quay", "", "", ""],
    "arcade":            ["Arcade of", "Echoes", "", ""],
    "stillhouse_row":    ["Stillhouse Row", "", "", ""],
    "tuners_lane":       ["Tuners' Lane", "", "", ""],
    "listeners_stair":   ["Listeners'", "Stair", "", ""],
    "starward_steps":    ["Starward Steps", "", "", ""],
    "garden_of_quiet":   ["Garden of", "Quiet", "", ""],
    "last_procession":   ["The Last", "Procession", "", ""],
    "soprano_gate":      ["SOPRANO GATE", "The high voice", "keeps watch", ""],
    "alto_gate":         ["ALTO GATE", "The warm voice", "welcomes", ""],
    "tenor_gate":        ["TENOR GATE", "The bright", "voice carries", ""],
    "bass_gate":         ["BASS GATE", "The deep voice", "holds", ""],
    "held_note_plaque":  ["HERE THE CHORD", "WAS HELD", "C.1 - C.1147", ""],
    "rest_plaque":       ["REST IS NOT", "AN ENDING", "", ""],
    "podium_plaque":     ["YSOLDE", "LAST CONDUCTOR", "", "SHE STAYED."],
    # Reawakening the Heart (docs/the-hush.md): the Podium's socket rail, the
    # Hall of Instruments' cabinet, the Archive's sealed cell, the Tuners'
    # mast, and the two plaques the server rewrites when the city awakes
    # (server/level/AurelithCities — the *_after texts, mirrored in
    # GeneratedAurelithBooks.inc).
    "podium_order":      ["FOUR VOICES", "FROM THE FLOOR", "TO THE CROWN", ""],
    "cabinet_plaque":    ["THE ALTO", "WELCOMES", "", "Sing it open"],
    "archive_seal":      ["SEALED", "C.1147", "", "- L."],
    "tuners_mast":       ["TENOR MAST", "The lighthouse", "note is set", "here"],
    "podium_plaque_after":    ["YSOLDE", "LAST CONDUCTOR", "SHE STAYED.", "WE CAME BACK."],
    "held_note_plaque_after": ["HERE THE CHORD", "WAS HELD", "C.1 - C.1147", "AND IS AGAIN"],
    "vesper_plaque":     ["VESPER", "What is sung", "is never lost", ""],
    "archive_door":      ["ARCHIVE OF", "ECHOES", "", "LISTEN FIRST"],
    "stillhouse_motto":  ["EVERY VOICE", "A LANTERN", "", ""],
    "hold_the_note":     ["HOLD THE NOTE", "", "", ""],
    "light_is_song":     ["LIGHT IS SONG", "MADE VISIBLE", "", ""],
    "scratched_note":    ["", "we will", "come back", ""],
    "tuners_works":      ["TUNERS' WORKS", "Conduits live", "Do not touch", "the rings"],
    "instruments_hall":  ["HALL OF", "INSTRUMENTS", "", "Tune softly"],
    "starward_door":     ["STARWARD", "Climb on a", "strong aurora", ""],
    "gardens_plaque":   ["THE QUIET", "GARDENS", "", "No talking."],
    "spire_door":        ["CONDUCTOR'S", "SPIRE", "", ""],
    "hesper_stall":      ["Hesper's Lamps", "Stall nine", "Retuning 1", "echo"],
    "quay_moorings":     ["Moor at posts", "NOT at lamps", "", "- Corvin"],
}

# ── The outskirts (tools/gen_aurelith_outskirts.py) ─────────────────────────
# The roads out of the gates, the Garden of Stones (the Choir's necropolis,
# off the Bass road) and the waystation on the Alto road. Kept apart from the
# city's own books and signs so the two generators never step on each other;
# merged into BOOKS / BOOK_HOMES / SIGNS below.
OUTSKIRTS_BOOKS: dict[str, dict] = {
    # ── The Rite of Rest (the necropolis' chapel lectern) ───────────────────
    "rite_of_rest": {
        "title": "The Rite of Rest",
        "author": "The Keepers of Rest",
        "pages": [
            "THE RITE OF REST\n\nas it is kept in\nthe Garden of\nStones, outside\nthe Bass Gate.\n\nFor the Keepers.\nRead it aloud only\nat the gate.",
            "I. We do not say\nthey died. We say\nthey have come to\ntheir rest, as a\nsong does.\n\nEvery song comes\nto one.",
            "II. Carry them out\nby the Bass Gate.\nThe deep voice\nholds the way down,\nand the deep voice\nwill hold them.",
            "III. At the tomb,\nlet the family hum\nthe note that was\ntheirs. Once, low,\nand not again.\n\nThe Hush keeps it.\nA note sung here\ndoes not fade.",
            "IV. Take the voice\nfrom the throat:\nthe small crystal\nthey sang through.\nIt goes back to\nthe Heart, and the\nHeart carries it\nin the Chord.",
            "V. Cut the name in\nthe stone. Leave\nthe face blank, as\non every statue we\nraise, so that\nanyone may sing\nin it.",
            "VI. Light the lamp\nat the foot of the\ntomb. It will burn\na hundred years.\nWhen it goes out\nthey are not gone.\nThey are only\nquiet.",
            "The Keeper says:\nREST IS NOT AN\nENDING.\n\nThe family says:\nWHAT IS SUNG IS\nNEVER LOST.\n\nThen everyone goes\nhome, softly.",
            "(another hand)\n\nC.1147. We carried\nno one out tonight.\nThe whole city is\nwalking out, and\nthere is no one\nleft to hum for us.\n\nHum for us.",
        ],
    },
    # ── The Waystation Book (the waystation's lectern) ──────────────────────
    "waystation_book": {
        "title": "The Waystation Book",
        "author": "Many Hands",
        "pages": [
            "THE WAYSTATION\nON THE ALTO ROAD\n\nTwo measures and a\nhalf from the Heart.\n\nWrite your name and\nyour road. Leave\nthe lamp lit.\n- the Keeper",
            "C.873. Pell of the\nQuays, walking to\nthe Choir Hall on\nthe steppe. Feet\nhurt. Soup good.\nThe Alto beam\nfollows you all\nnight. Kind of it.",
            "C.1021. Two Tuners\nout to the\nlighthouses with\nnew crystal. The\nfar lamps drift\nflat. We tune them\nfrom the Tenor.\nAlways the Tenor.",
            "C.1144. A family\nfrom Stillhouse\nRow, going nowhere\nin particular. The\nchildren will not\nsleep in the city.\nThey dream of a\ndeep place. Here\nthey slept.",
            "C.1146. Nobody\nwalked the road\nthis month. The\nKeeper says the\nlighthouses turn\nwrong now. They\nstop, and look back\ntoward the city.",
            "C.1147. The whole\ncity passed in the\nnight. Nobody\nstopped. Nobody\nwrote. I am\nwriting it for\nthem.\n\nThe lamp is lit.\n- the Keeper",
        ],
    },
}

OUTSKIRTS_BOOK_HOMES: dict[str, list[str]] = {
    "necropolis": ["rite_of_rest"],
    "waystation": ["waystation_book", "legend"],
}

# The mile-markers: a stone every measure (a hundred paces — the city is one
# measure from its Heart to its wall, so the first stone outside a gate says
# II), on the right of the road going out, its face cut in the Stave and the
# words beneath. mile_<voice>_<measures>.
OUTSKIRTS_SIGNS: dict[str, list[str]] = {
    "mile_soprano_2":    ["SOPRANO ROAD", "AURELITH", "II MEASURES", "KEEP WATCH"],
    "mile_soprano_3":    ["SOPRANO ROAD", "AURELITH", "III MEASURES", "LISTEN FIRST"],
    "mile_soprano_4":    ["SOPRANO ROAD", "AURELITH", "IV MEASURES", "HOLD THE NOTE"],
    "mile_alto_2":       ["ALTO ROAD", "AURELITH", "II MEASURES", "YOU ARE WELCOME"],
    "mile_alto_3":       ["ALTO ROAD", "AURELITH", "III MEASURES", "EVERY VOICE"],
    "mile_alto_4":       ["ALTO ROAD", "AURELITH", "IV MEASURES", "A LANTERN"],
    "mile_tenor_2":      ["TENOR ROAD", "AURELITH", "II MEASURES", "IT CARRIES"],
    "mile_tenor_3":      ["TENOR ROAD", "AURELITH", "III MEASURES", "FOLLOW THE BEAM"],
    "mile_tenor_4":      ["TENOR ROAD", "AURELITH", "IV MEASURES", "LIGHT IS SONG"],
    "mile_bass_2":       ["BASS ROAD", "AURELITH", "II MEASURES", "THE DEEP HOLDS"],
    "mile_bass_3":       ["BASS ROAD", "AURELITH", "III MEASURES", "REST, NOT END"],
    "mile_bass_4":       ["BASS ROAD", "AURELITH", "IV MEASURES", "HUM ONCE"],
    # The Garden of Stones.
    "garden_of_stones":  ["THE GARDEN", "OF STONES", "", "Hum once."],
    "tomb_aldric":       ["HERE RESTS", "ALDRIC OF", "THE TUNERS", "C.388 - C.451"],
    "tomb_bryony":       ["HERE RESTS", "BRYONY OF THE", "LAMPWRIGHTS", "C.512 - C.590"],
    "tomb_cassian":      ["HERE RESTS", "CASSIAN OF", "THE QUAYS", "C.640 - C.702"],
    "tomb_delphine":     ["HERE RESTS", "DELPHINE OF THE", "LISTENERS", "C.771 - C.839"],
    "tomb_emeric":       ["HERE RESTS", "EMERIC OF", "THE STARWARD", "C.903 - C.960"],
    "tomb_fenna":        ["HERE RESTS", "FENNA OF THE", "STILLHOUSES", "HER NOTE HOLDS"],
    "tomb_gideon":       ["HERE RESTS", "GIDEON OF", "THE WARDENS'", "KEEPERS"],
    "tomb_hollis":       ["HERE RESTS", "HOLLIS, AGED 6", "", "A SMALL VOICE"],
    "tomb_first":        ["THE FIRST", "CONDUCTOR", "", "C.1 - C.61"],
    "tomb_severin":      ["SEVERIN", "CONDUCTOR", "HIS NOTE HOLDS", "C.402 - C.470"],
    "tomb_ismay":        ["ISMAY", "CONDUCTOR", "BEFORE YSOLDE", "C.1060 - C.1129"],
    "tomb_empty":        ["", "(the name is", "not cut yet)", ""],
    # The waystation and the roads.
    "waystation_door":   ["WAYSTATION", "Alto Road", "", "Leave the lamp"],
    "signpost_city":     ["<- AURELITH", "two measures", "and a half", ""],
    "signpost_steppe":   ["THE STEPPE ->", "Choir Hall", "lighthouses", ""],
    "roadside_shrine":   ["REST AND", "LISTEN", "", ""],
    "procession_arch":   ["THE LAST", "PROCESSION", "C.1147", "passed here"],
}

BOOKS.update(OUTSKIRTS_BOOKS)
BOOK_HOMES.update(OUTSKIRTS_BOOK_HOMES)
SIGNS.update(OUTSKIRTS_SIGNS)

# ── self-check ───────────────────────────────────────────────────────────────
PAGE_CHARS = 220
PAGE_LINES = 14
LINE_CHARS = 19
SIGN_CHARS = 15
TITLE_CHARS = 32   # WrittenBookContent title: Codec.string(0, 32)


def wrap_lines(page: str, width: int = LINE_CHARS) -> int:
    """Greedy word-wrap line count; every explicit newline (and blank line)
    counts as a line, a word longer than `width` is split."""
    total = 0
    for para in page.split("\n"):
        words = para.split()
        if not words:
            total += 1
            continue
        line = 0
        for word in words:
            while len(word) > width:
                if line:
                    total += 1
                    line = 0
                total += 1
                word = word[width:]
            if line == 0:
                line = len(word)
            elif line + 1 + len(word) <= width:
                line += 1 + len(word)
            else:
                total += 1
                line = len(word)
        total += 1
    return total


def check() -> list[str]:
    errors: list[str] = []
    for key, book in BOOKS.items():
        title, author, pages = book["title"], book["author"], book["pages"]
        if not 0 < len(title) <= TITLE_CHARS:
            errors.append(f"{key}: title length {len(title)}")
        if not author.strip():
            errors.append(f"{key}: empty author")
        if not 3 <= len(pages) <= 12:
            errors.append(f"{key}: {len(pages)} pages")
        for i, page in enumerate(pages, 1):
            if not page.strip():
                errors.append(f"{key} p{i}: empty")
            if len(page) > PAGE_CHARS:
                errors.append(f"{key} p{i}: {len(page)} chars")
            n = wrap_lines(page)
            if n > PAGE_LINES:
                errors.append(f"{key} p{i}: {n} lines")
    for home, keys in BOOK_HOMES.items():
        for k in keys:
            if k not in BOOKS:
                errors.append(f"BOOK_HOMES[{home}]: unknown book {k}")
    homed = {k for keys in BOOK_HOMES.values() for k in keys}
    for k in BOOKS:
        if k not in homed:
            errors.append(f"{k}: in no BOOK_HOMES list")
    if not 20 <= len(SIGNS) <= 120:
        errors.append(f"{len(SIGNS)} signs")
    for key, lines in SIGNS.items():
        if len(lines) != 4:
            errors.append(f"sign {key}: {len(lines)} lines")
        for line in lines:
            if len(line) > SIGN_CHARS:
                errors.append(f"sign {key}: '{line}' is {len(line)} chars")
    return errors


# ── The engine's copy ───────────────────────────────────────────────────────
# What the SERVER writes at run time rather than a template carrying it: the
# Coda (set open on the Conductor's seat when the city awakes) and the two
# plaques rewritten then. Emitted as a generated C++ include so the words stay
# here, in one place:
#
#   python3 tools/aurelith_books.py --emit-cpp     # -> GeneratedAurelithBooks.inc
#   python3 tools/aurelith_books.py --check-cpp    # exit 1 when it has drifted
ENGINE_BOOKS = ["coda"]
ENGINE_SIGNS = ["podium_plaque_after", "held_note_plaque_after"]
ENGINE_INC = "src/common/world/level/GeneratedAurelithBooks.inc"


def _c_string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def emit_cpp() -> str:
    lines = [
        "// GENERATED by tools/aurelith_books.py --emit-cpp — DO NOT EDIT BY HAND.",
        "// The Aurelith words the server writes at run time (the Coda, the rewritten",
        "// plaques); the text itself lives in tools/aurelith_books.py.",
        "//",
        "// AURELITH_BOOK(key, title, author, pages...) / AURELITH_SIGN(key, l0, l1, l2, l3)",
        "",
    ]
    for key in ENGINE_BOOKS:
        b = BOOKS[key]
        pages = ",\n    ".join(_c_string(pg) for pg in b["pages"])
        lines.append(f"AURELITH_BOOK({key}, {_c_string(b['title'])}, {_c_string(b['author'])},\n    {pages})")
    for key in ENGINE_SIGNS:
        lines.append(f"AURELITH_SIGN({key}, " + ", ".join(_c_string(l) for l in SIGNS[key]) + ")")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    if "--emit-cpp" in sys.argv or "--check-cpp" in sys.argv:
        from pathlib import Path
        out = Path(__file__).resolve().parent.parent / ENGINE_INC
        text = emit_cpp()
        if "--check-cpp" in sys.argv:
            ok = out.exists() and out.read_text() == text
            print(f"{ENGINE_INC}: " + ("up to date" if ok else "DRIFTED - rerun --emit-cpp"))
            sys.exit(0 if ok else 1)
        out.write_text(text)
        print(f"wrote {ENGINE_INC}")
    errs = check()
    for key, book in BOOKS.items():
        print(f"{key:12s} {len(book['pages']):2d} pages  {book['title']!r} by {book['author']}")
    print(f"{len(BOOKS)} books, {sum(len(b['pages']) for b in BOOKS.values())} pages, "
          f"{len(SIGNS)} signs, {len(BOOK_HOMES)} homes")
    if errs:
        print("FAILED:")
        for e in errs:
            print("  " + e)
        sys.exit(1)
    print("self-check: OK")

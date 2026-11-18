// Papertrail's own script around citeproc-js, run after it in the same QuickJS context (see CslEngine.cpp). It adds
// an output format that marks styling with private-use characters, so C++ never has to read HTML, a faster way to
// find macros, and the few functions C++ calls. Nothing here can reach files or the network: QuickJS is built
// without its standard library, and the only thing C++ provides is __ptLocale, which returns a bundled locale.
var CSL = module.exports;

// citeproc-js reports warnings through console or dump, which this host doesn't have.
var console = {log: function () {}, warn: function () {}, error: function () {}};
function dump() {}

// CSL only allows macros as direct children of cs:style, so a macro is looked for there, rather than by walking
// the whole style for each one, which made APA take seconds to load.
(function () {
    var walk = CSL.XmlJSON.prototype.getNodesByName;
    CSL.XmlJSON.prototype.getNodesByName = function (myjson, name, nameattrval, ret) {
        if (!ret && name === "macro" && nameattrval && myjson && myjson.name === "style" && myjson.children) {
            var found = [];
            for (var i = 0; i < myjson.children.length; i++) {
                var c = myjson.children[i];
                if (c && "object" === typeof c && c.name === "macro" && c.attrs && c.attrs.name === nameattrval)
                    found.push(c);
            }
            return found;
        }
        return walk.call(this, myjson, name, nameattrval, ret);
    };
}());

// A run opens with U+E000 and a letter and closes with U+E001: i italic, I upright, b bold, B normal weight,
// c small capitals, C normal letters, u superscript, d subscript, n the baseline. A numeric style's left margin,
// such as "[1]", is wrapped in U+E002 and U+E003. Any of these in a publication's details are removed first, so
// a title can't style itself.
function ptMark(letter) {
    return "" + letter + "%%STRING%%";
}
CSL.Output.Formats.papertrail = {
    "text_escape": function (text) {
        if (!text) {
            text = "";
        }
        return text.replace(/[-]/g, "");
    },
    "bibstart": "",
    "bibend": "",
    "@font-style/italic": ptMark("i"),
    "@font-style/oblique": ptMark("i"),
    "@font-style/normal": ptMark("I"),
    "@font-variant/small-caps": ptMark("c"),
    "@passthrough/true": CSL.Output.Formatters.passthrough,
    "@font-variant/normal": ptMark("C"),
    "@font-weight/bold": ptMark("b"),
    "@font-weight/normal": ptMark("B"),
    "@font-weight/light": false,
    "@text-decoration/none": false,
    "@text-decoration/underline": false,
    "@vertical-align/sup": ptMark("u"),
    "@vertical-align/sub": ptMark("d"),
    "@vertical-align/baseline": ptMark("n"),
    "@strip-periods/true": CSL.Output.Formatters.passthrough,
    "@strip-periods/false": CSL.Output.Formatters.passthrough,
    "@quotes/true": function (state, str) {
        if ("undefined" === typeof str) {
            return state.getTerm("open-quote");
        }
        return state.getTerm("open-quote") + str + state.getTerm("close-quote");
    },
    "@quotes/inner": function (state, str) {
        if ("undefined" === typeof str) {
            return "’";
        }
        return state.getTerm("open-inner-quote") + str + state.getTerm("close-inner-quote");
    },
    "@quotes/false": false,
    "@cite/entry": function (state, str) {
        return str;
    },
    "@bibliography/entry": function (state, str) {
        return str;
    },
    "@display/block": function (state, str) {
        return str;
    },
    "@display/left-margin": function (state, str) {
        return "" + str + "";
    },
    "@display/right-inline": function (state, str) {
        return str;
    },
    "@display/indent": function (state, str) {
        return str;
    },
    "@showid/true": function (state, str) {
        return str;
    },
    "@URL/true": function (state, str) {
        return str;
    },
    "@DOI/true": function (state, str) {
        return str;
    }
};

var ptEngine = null;
var ptItems = {};
var ptSys = {
    retrieveLocale: function (lang) {
        return __ptLocale(String(lang));
    },
    retrieveItem: function (id) {
        return ptItems[id];
    },
    // QuickJS's localeCompare ignores the options citeproc-js relies on, so Papertrail compares instead.
    stringCompare: function (a, b) {
        return __ptCompare(String(a), String(b));
    }
};

// Makes the engine for a style, given as its XML. `lang` is the locale to use, and `force` makes it win over the
// style's own default-locale.
function ptSetStyle(xml, lang, force) {
    ptEngine = null;
    ptItems = {};
    var engine = new CSL.Engine(ptSys, xml, lang || "en-US", !!force);
    engine.setOutputFormat("papertrail");
    ptEngine = engine;
    return "";
}

function ptUse(itemsJson) {
    var items = JSON.parse(itemsJson);
    var ids = [];
    ptItems = {};
    for (var i = 0; i < items.length; i++) {
        ptItems[items[i].id] = items[i];
        ids.push(items[i].id);
    }
    ptEngine.updateItems(ids);
    return ids;
}

// The reference list for these items, in the style's order, as JSON: {ids: [id, ...], entries: [text, ...]}, or
// {none: true} for a style without a bibliography.
function ptBibliography(itemsJson) {
    ptUse(itemsJson);
    var bib = ptEngine.makeBibliography();
    if (!bib) {
        return JSON.stringify({none: true});
    }
    var ids = [];
    for (var i = 0; i < bib[0].entry_ids.length; i++) {
        ids.push(String(bib[0].entry_ids[i][0]));
    }
    return JSON.stringify({ids: ids, entries: bib[1]});
}

// One citation of all these items together. `note` is true for a note style, whose citation is its note.
function ptCitation(itemsJson, note) {
    var ids = ptUse(itemsJson);
    var cited = [];
    for (var i = 0; i < ids.length; i++) {
        cited.push({id: ids[i]});
    }
    return ptEngine.previewCitationCluster({citationItems: cited, properties: {noteIndex: note ? 1 : 0}}, [], [],
                                           "papertrail");
}

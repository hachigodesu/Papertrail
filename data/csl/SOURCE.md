# CSL styles and locales

The citation styles in `styles/` and the locales in `locales/` come from the Citation Style Language project, and are built into Papertrail so that CSL styles work without anything fetched online.

| | Repository | Branch | Commit |
|---|---|---|---|
| Styles | https://github.com/citation-style-language/styles | `v1.0.2` | `7a37c5a736c10fbd468bc31f92cbc6eafaa2173d` |
| Locales | https://github.com/citation-style-language/locales | `v1.0.2` | `f637c83f6267b761513bcc1cb0e95571172e768c` |

Both are licensed under the Creative Commons Attribution-ShareAlike 3.0 Unported licence, https://creativecommons.org/licenses/by-sa/3.0/. Each style keeps its authors, contributors and licence in its own `<info>`, unchanged.

`tools/dev/update-csl.py` fetches them again at the commits it names, with each dependent style's parent, after its list of styles or its commits are changed. Change the table above to match.

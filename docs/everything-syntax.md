# Everything search language

The query language esidx must parse. Line references of the form `L<n>` point
into the original document, which shipped with etp_server as `搜索语法` and is
Everything's own documentation; it has been reformatted as tables here, with
section headings kept in the order of the original.

The ETP client drives this through `SITE EVERYTHING`
subcommands rather than by sending a raw query string: it sets match options
(`CASE`, `PATH`, `REGEX`, `WHOLE_WORD`, …), column toggles (`SIZE_COLUMN`,
`PATH_COLUMN`, …), then `OFFSET`, `COUNT`, `SORT` and finally `QUERY`. The
server turns those into a query internally. See [`design.md`](design.md) §1 for
the subcommand list and §6 for the execution model.

**What this document does not settle.** Everything's own documentation does not
say which text an *unqualified* search term is matched against, and it does not
say what a wildcard inside a `path:` value does. Both had to be measured against
voidtools' own server rather than read off: design §12.10 has the table, and
`./cmp_ref.sh` re-measures every row of it. In one line — a term with no
separator in it matches the **filename**, a term with a separator in it (or an
explicit `path:`) matches the **path**, and `path:` with a value that starts with
a star is a "contains anywhere in the path" test.

A separator there is anyone's: `/`, `\` and `\\` all delimit components, and a run
of them is one separator. The client joins `path + "\" + name`, so a POSIX
`/work/sub` comes back as `/work\sub`, and one more layer of quoting -- or a
Windows habit -- makes that `sub1\\x.conf`; all of them name the same path, so the
value is collapsed into one spelling before it is matched. The exception is
`regex:`, where `\` is the escape it is in every other regex and `\\` is a literal
backslash: a pattern is never rewritten, and the path it has to match is offered in
the other spelling instead. design §12.9/§12.10 carry the measurements, `test.sh`
the rows.

It also does not say whether a macro, or a `file:`/`folder:` modifier, may be
*followed* by a term. That one changes the whole result set rather than a row:
`folder:abc` is the folder filter **AND** the term `abc`, not "the folders, and
`abc` is somebody else's business". Measured on the reference (`:21`, the probe
scripts' shape; the nonsense rows are in `cmp_ref.sh` and re-measurable):

| Term | Reference | Reads as |
|---|---|---|
| `folder:` | 1 594 989 | the filter alone |
| `folder:zzzznotfound` | 0 | the value is a term, and it is required |
| `image:` / `image:zzzznotfound` | 422 143 / 0 | the macros behave the same way |
| `root:` / `root:zzzznotfound` | 7 / 0 | so do the names that read like argument-less functions |
| `empty:` / `empty:zzzznotfound` | 141 171 / 0 | |
| `folder:zzzznotfound zzzznotfound` | 0 | which is what AND means here |
| `!folder:zzzznotfound` | 12 255 166 | the complement of the row above: `!` negates the whole term, modifier and value together |

`case:folder:zzzznotfound` and `folder:path:zzzznotfound` are 0 as well, so a
modifier in front of the prefix scopes that same term. Two consequences worth
writing down, because both were wrong here: `file:.txt` is not `file:` plus noise
(it selects the `.txt` files, and `file:` alone selects every file), and
`folder:abc` is not the same query as `abc folder:` only by accident — they are
the same query by grammar.

---

## Operators — L2-8

| Token | Meaning |
|---|---|
| `space` | AND |
| `\|` | OR |
| `!` | NOT |
| `< >` | grouping |
| `" "` | escape an operator character |

## Wildcards — L10-15

| Token | Meaning |
|---|---|
| `*` | any characters, 0 or more (except `\`) |
| `**` | any characters, 0 or more |
| `?` | any one character (except `\`) |

When wildcards are used the match applies to the **whole** filename.

## Character entities — L17-26

| Token | Meaning |
|---|---|
| `&sp;` | space (` `) |
| `&vert;` | vertical bar (`\|`) |
| `&excl;` | exclamation mark (`!`) |
| `lt:` | less-than (`<`) |
| `gt:` | greater-than (`>`) |
| `quot:` | double quote (`"`) |
| `#<n>` | decimal Unicode character `<n>` |
| `#x<n>` | hexadecimal Unicode character `<n>` |

## Macros — L28-35

| Macro | Meaning |
|---|---|
| `audio:` | audio files |
| `zip:` | archives |
| `doc:` | documents |
| `exe:` | executables |
| `image:` | images |
| `video:` | video files |

## Modifiers — L37-63

Modifiers apply to search functions and may be combined. Prefix with `no` to
disable, or with `?` to enable globally.

| Modifier | Meaning |
|---|---|
| `binary:` | match as a byte stream; `\\` = `\`, `\xff` = hex byte |
| `case:` | case-sensitive matching |
| `diacritics:` | match diacritics |
| `endwith:` | match the end of the filename or property |
| `file:` | files only |
| `folder:` | folders only |
| `hex:` | convert pairs of hex characters to bytes |
| `highlight:` | highlight the search term |
| `ignorepunc:` | ignore punctuation in the filename or property |
| `ignorews:` | ignore whitespace in the filename or property |
| `len:` | match filename length, or hex property characters |
| `fromdisk:` | search disk contents and properties |
| `tonumber:` | convert text to a number before comparing |
| `path:` | match the full path |
| `prefix:` | match the start of a word |
| `regex:` | enable regular expressions |
| `startwith:` | match the start of the filename or property |
| `suffix:` | match the end of a word. **The reference has no such function**: it drops the modifier and matches the bare term (measured, `AGENTS.md` §5.2) |
| `tostring:` | convert a property to its display string before comparing |
| `whole:` | match the whole filename or property |
| `ww:` | whole words only. `_` is a boundary, not a word character: `ww:esidx` matches `esidx_x` |
| `wildcards:` | automatically treat the content as a wildcard pattern |

## Wildcard syntax inside search modifiers — L65-72

| Token | Meaning |
|---|---|
| `*` | any characters, 0 or more |
| `?` | any one character |
| `#` | any single digit (`0`-`9`) |
| `[ ]` | any character in the brackets |
| `[! ]` | any character not in the brackets |
| `\` | escape the next character |

## Functions — L74-114

| Function | Meaning |
|---|---|
| `album:` | media album metadata |
| `artist:` | media artist metadata |
| `attrib:` | files and folders with the given attributes |
| `bitdepth:` | media files with the given pixel depth |
| `child:` | folders containing a file with the given name |
| `child-count:` | folders with the given number of subfolders + files |
| `child-file-count:` | folders with the given number of files |
| `child-folder-count:` | folders with the given number of subfolders |
| `comment:` | media comment metadata |
| `content:` | file content |
| `count:` | maximum number of search results |
| `dc:` | files and folders with the given creation date |
| `depth:` | files and folders with the given number of parent folders |
| `dm:` | files and folders with the given modification date |
| `dupe:` | files and folders with duplicate properties |
| `empty:` | empty folders |
| `ext:` | files with the given extension(s), `;`-separated |
| `filelist:` | names in the given `;`-separated list |
| `filelist-filename:` | files and folders in the given filename list |
| `frn:` | files and folders with the given file index number |
| `genre:` | media genre metadata |
| `height:` | media files with the given pixel height |
| `index-type:` | files and folders with the given index type |
| `length:` | media files with the given duration |
| `name:` | files and folders containing the given name part |
| `orientation:` | images with the given orientation |
| `parent:` | files and folders under the given path (excluding subfolders) |
| `path-part:` | files and folders containing the given path part |
| `root:` | files and folders with no parent folder |
| `run-count:` | files and folders with the given open count |
| `si:` | use the system index with the advanced query syntax |
| `size:` | files with the given size, in bytes |
| `star-rating:` | media files rated 1-5 |
| `stem:` | files and folders containing the given name stem |
| `tag:` | media tag metadata |
| `title:` | media title metadata |
| `track:` | media track metadata |
| `type:` | files and folders of the given type |
| `width:` | media files with the given pixel width |

## Function comparison syntax — L116-131

| Form | Meaning |
|---|---|
| `function:value` | equal |
| `function:<=value` | less than or equal |
| `function:<value` | less than |
| `function:=value` | equal |
| `function:==value` | equal |
| `function:>value` | greater than |
| `function:>=value` | greater than or equal |
| `function:!=value` | not equal |
| `function:!value` | not equal, at the given granularity |
| `function:start..end` | within the range |
| `function:start-end` | within the range |
| `function:<val1 val2>` | val1 **and** val2 |
| `function:<val1 \| val2>` | val1 **or** val2 |
| `function:val1;val2` | val1 **or** val2 |

## Size syntax and constants — L133-146

Sizes accept the suffixes `kb`, `mb`, `gb`.

| Constant | Range |
|---|---|
| `empty` | 0 bytes |
| `tiny` | 0 KB < size ≤ 10 KB |
| `small` | 10 KB < size ≤ 100 KB |
| `medium` | 100 KB < size ≤ 1 MB |
| `large` | 1 MB < size ≤ 16 MB |
| `huge` | 16 MB < size ≤ 128 MB |
| `gigantic` | size > 128 MB |
| `unknown` | — |

## Duration syntax and constants — L148-164

```
[[[DD:]HH:]MM:]SS[.sss]
[<n><d|day|days>][<n><h|hour|hours>][<n><m|min|mins|minute|minutes>][<n><s|sec|secs|second|seconds>]
P[n]Y[n]M[n]DT[n]H[n]M[n]S
P[n]W
PYYYYMMDDThhmmss
P[YYYY]-[MM]-[DD]T[hh]:[mm]:[ss]
```

| Constant | Range |
|---|---|
| `very-short` | under 1 minute |
| `short` | 1-5 minutes |
| `medium` | 5-30 minutes |
| `long` | 30-60 minutes |
| `very-long` | over 60 minutes |
| `unknown` | — |

## Date syntax and constants — L166-189

```
year
month/year or year/month                    (locale dependent)
day/month/year, month/day/year or year/month/day   (locale dependent)
YYYY[-MM[-DD[Thh[:mm[:ss[.sss]]]]]]
YYYYMMDD[Thh[mm[ss[.sss]]]]]
YYMMDD
FILETIME (>99999999)
```

| Constant | Meaning |
|---|---|
| `today`, `yesterday` | |
| `<last\|current\|this\|coming\|next><year\|month\|week>` | |
| `<last\|coming\|next><x><years\|months\|weeks\|days\|hours\|minutes\|mins\|seconds\|secs>` | |
| `past[x]<year[s]\|month[s]\|week[s]\|day[s]\|hour[s]\|minute[s]\|min[s]\|second[s]\|sec[s]>` | |
| `<prev\|previous>[x]<year[s]\|month[s]\|week[s]>` | |
| `january`…`december`, `jan`…`dec` | |
| `sunday`…`saturday`, `sun`…`sat` | |
| `mtd`, `ytd` | month-to-date, year-to-date |
| `unknown` | |

## Attribute constants — L191-209

| Constant | Attribute |  | Constant | Attribute |
|---|---|---|---|---|
| `A` | archive |  | `M` | recall on access |
| `C` | compressed |  | `N` | normal |
| `D` | directory |  | `O` | offline |
| `E` | encrypted |  | `P` | sparse file |
| `H` | hidden |  | `R` | read-only |
| `I` | not content-indexed |  | `S` | system |
| `L` | reparse point |  | `T` | temporary |
| | |  | `U` | online-only |
| | |  | `V` | integrity stream |
| | |  | `X` | non-erasable |

On ext4 we can populate `H` (leading dot) and `D` (from `d_type`); the rest
have no filesystem equivalent and must return "unsupported".

## Filename components — L211-217

| Component | Example |
|---|---|
| full path | `C:\folder\file.txt` |
| name | `file.txt` |
| path | `C:\folder` |
| stem | `file` |
| extension | `txt` |

## Examples — L219-243

```
c:                                    files and folders on drive C:
d:|e:                                 files and folders on D: or E:
!f:                                   files and folders not on F:
\downloads                             everything under the Downloads folder
!\appdata                             everything not under appdata
c:\windows\                            everything under C:\Windows
"c:\Program Files\"                    quoted path with a space
size:>1gb                              larger than 1 GB
size:10mb..50mb                        between 10 MB and 50 MB
size:<1mb                              smaller than 1 MB
dm:today                               modified today
dm:>2022                               modified after 2022
dm:2021-07..2022-06                    modified between 2021-07 and 2022-06
dm:<2020                               modified before 2020
*.mp3                                  mp3 files
ext:jpg;png                            jpg or png
attrib:h                               hidden
length:>1hour                          longer than an hour
length:10mins..30mins                  between 10 and 30 minutes
length:<1min                           under a minute
content:abc                            content contains abc
content:<abc 123>                      content contains abc and 123
regex:content:^foo                     content starts with foo
```
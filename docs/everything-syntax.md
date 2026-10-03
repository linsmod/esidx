# Everything search language

The query language esidx must parse. Line references of the form `L<n>` point
into the original document, which shipped with etp_server as `搜索语法` and is
Everything's own documentation; it has been reformatted as tables here, with
section headings kept in the order of the original.

The Android client (ShareToPC) drives this through `SITE EVERYTHING`
subcommands rather than by sending a raw query string: it sets match options
(`CASE`, `PATH`, `REGEX`, `WHOLE_WORD`, …), column toggles (`SIZE_COLUMN`,
`PATH_COLUMN`, …), then `OFFSET`, `COUNT`, `SORT` and finally `QUERY`. The
server turns those into a query internally. See [`design.md`](design.md) §1 for
the subcommand list and §6 for the execution model.

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
| `suffix:` | match the end of a word |
| `tostring:` | convert a property to its display string before comparing |
| `whole:` | match the whole filename or property |
| `ww:` | whole words only |
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
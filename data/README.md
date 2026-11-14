# Data

Daily bars of seven US stocks, from the Quandl WIKI Prices dataset, as [MarketData.jl](https://github.com/JuliaQuant/MarketData.jl) publishes it. The WIKI dataset was curated by Quandl's community and released into the public domain; MarketData.jl [documents its data](https://juliaquant.github.io/MarketData.jl/stable/) as "public domain without restrictions". [`tools/prepare_data.py`](../tools/prepare_data.py) downloads the files at a pinned commit and writes them here, and `--check` requires a fresh download to reproduce them byte for byte.

| Symbol | Company | Bars | From | To | Notes |
| --- | --- | ---: | --- | --- | --- |
| AAPL | Apple | 8,336 | 1980-12-12 | 2013-12-31 | The 52% fall of 2000-09-29, after a profit warning, is real |
| BA | Boeing | 13,090 | 1962-01-02 | 2013-12-31 | |
| CAT | Caterpillar | 13,090 | 1962-01-02 | 2013-12-31 | The source misses the 2-for-1 split of 1964-07-06; the bars before it are adjusted here |
| DELL | Dell | 6,353 | 1988-08-17 | 2013-10-29 | Ends when Dell was taken private |
| EBAY | eBay | 3,842 | 1998-09-24 | 2013-12-31 | Starts the day it listed |
| F | Ford | 10,491 | 1972-06-01 | 2013-12-31 | The 32% fall of 1998-04-08 is the spin-off of Associates First Capital, which the source doesn't adjust for |
| GE | General Electric | 13,090 | 1962-01-02 | 2013-12-31 | |

Each file has the columns `Date,Open,High,Low,Close,Volume`, oldest bar first. Prices are adjusted for splits and dividends, so a buy-and-hold return on them is a total return, and are kept to six significant digits; volumes are split-adjusted. Three bars in the source have a high or low that doesn't contain the open and close (BA on 1962-01-23, CAT on 1963-01-11 and 1968-02-07). They are left as they are, and NanoTrade's loader repairs them, widening the range, and reports it.

**What the data leaves open.** These are seven companies that still traded, or were taken private, at the end of 2013, picked in hindsight, so a strategy tested on them alone inherits survivorship bias. The dataset ends in March 2018 and these files at the end of 2013. Adjusted prices before the 1980s are fractions of a dollar, so their last digits carry little meaning. And the bars are what traded, not what a backtest could have traded at: there is no bid, ask, or depth.

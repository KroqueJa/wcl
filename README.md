# WCL

Have you ever used `wc -l`? I have. If you are running said command on a processor modern enough to support vectorized instructions and multithreading (`wc` does those things too), `wcl` is faster.

Is it "blazingly fast"? ~~Probably not yet~~ If it works the way it seems to work... yes! In my preliminary testing (on one system) it is

- 40% faster than `wc -l` on a very big file
- 69% faster on a "typical" dataset consisting of some random large csv files from kaggle
- 300% (!) faster on a dataset consisting of 4720 json files



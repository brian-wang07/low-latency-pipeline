benchmark results - 20M messages, 1ns interval per message, ticker NVDA
results are written to latency.log in build directory

commit eb5baf2
```
=== aggregated (n=20000000) ===
[lat] transit p50=205ns p99=205ns p999=840.0us max=14.7ms n=20000000
[lat] process p50=26ns p99=26ns p999=51ns max=14.7ms n=20000000
[lat] e2e p50=205ns p99=205ns p999=840.0us max=14.7ms n=20000000

=== full distribution ===
[lat-full] transit n=20000000 max=14.7ms
  [ 7]     51ns..103ns    n=3918443    cum= 19.59%
  [ 8]    103ns..205ns    n=16026110   cum= 99.72%
  [ 9]    205ns..410ns    n=1712       cum= 99.73%
  [10]    410ns..820ns    n=868        cum= 99.74%
  [11]    820ns..1.6us    n=225        cum= 99.74%
  [12]    1.6us..3.3us    n=547        cum= 99.74%
  [13]    3.3us..6.6us    n=774        cum= 99.74%
  [14]    6.6us..13.1us   n=844        cum= 99.75%
  [15]   13.1us..26.2us   n=732        cum= 99.75%
  [16]   26.2us..52.5us   n=1222       cum= 99.76%
  [17]   52.5us..105.0us  n=2379       cum= 99.77%
  [18]  105.0us..210.0us  n=4745       cum= 99.79%
  [19]  210.0us..420.0us  n=9625       cum= 99.84%
  [20]  420.0us..840.0us  n=15020      cum= 99.92%
  [21]  840.0us..1.7ms    n=10277      cum= 99.97%
  [22]    1.7ms..3.4ms    n=2381       cum= 99.98%
  [23]    3.4ms..6.7ms    n=992        cum= 99.98%
  [24]    6.7ms..13.4ms   n=2607       cum=100.00%
  [25]   13.4ms..26.9ms   n=497        cum=100.00%
[lat-full] process n=20000000 max=14.7ms
  [ 5]     13ns..26ns     n=19883762   cum= 99.42%
  [ 6]     26ns..51ns     n=99886      cum= 99.92%
  [ 7]     51ns..103ns    n=368        cum= 99.92%
  [ 8]    103ns..205ns    n=572        cum= 99.92%
  [ 9]    205ns..410ns    n=2141       cum= 99.93%
  [10]    410ns..820ns    n=3065       cum= 99.95%
  [11]    820ns..1.6us    n=8861       cum= 99.99%
  [12]    1.6us..3.3us    n=702        cum=100.00%
  [13]    3.3us..6.6us    n=185        cum=100.00%
  [14]    6.6us..13.1us   n=205        cum=100.00%
  [15]   13.1us..26.2us   n=18         cum=100.00%
  [16]   26.2us..52.5us   n=1          cum=100.00%
  [20]  420.0us..840.0us  n=233        cum=100.00%
  [25]   13.4ms..26.9ms   n=1          cum=100.00%
[lat-full] e2e n=20000000 max=14.7ms
  [ 7]     51ns..103ns    n=1256166    cum=  6.28%
  [ 8]    103ns..205ns    n=18673639   cum= 99.65%
  [ 9]    205ns..410ns    n=1801       cum= 99.66%
  [10]    410ns..820ns    n=3887       cum= 99.68%
  [11]    820ns..1.6us    n=10581      cum= 99.73%
  [12]    1.6us..3.3us    n=1396       cum= 99.74%
  [13]    3.3us..6.6us    n=886        cum= 99.74%
  [14]    6.6us..13.1us   n=968        cum= 99.75%
  [15]   13.1us..26.2us   n=805        cum= 99.75%
  [16]   26.2us..52.5us   n=1228       cum= 99.76%
  [17]   52.5us..105.0us  n=2381       cum= 99.77%
  [18]  105.0us..210.0us  n=4746       cum= 99.79%
  [19]  210.0us..420.0us  n=9620       cum= 99.84%
  [20]  420.0us..840.0us  n=15059      cum= 99.92%
  [21]  840.0us..1.7ms    n=10348      cum= 99.97%
  [22]    1.7ms..3.4ms    n=2392       cum= 99.98%
  [23]    3.4ms..6.7ms    n=992        cum= 99.98%
  [24]    6.7ms..13.4ms   n=2607       cum=100.00%
  [25]   13.4ms..26.9ms   n=498        cum=100.00%
  ```
benchmark results - 20M messages, 1ns interval per message, ticker NVDA
results are written to latency.log in build directory
conditions: fresh boot, no other processes running, feed pre-read with cat itch_feed/S071321-v50.txt > /dev/null

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

  commit 714ea2b
  ```
  === aggregated (n=20000000 drops=1) ===
[lat] transit p50=205ns p99=3.3us p999=839.9us max=4.9ms n=20000000
[lat] process p50=26ns p99=26ns p999=51ns max=3.0ms n=20000000
[lat] e2e p50=205ns p99=3.3us p999=839.9us max=4.9ms n=20000000

=== full distribution ===
[lat-full] transit n=20000000 max=4.9ms
  [ 7]     51ns..103ns    n=4798809    cum= 23.99%
  [ 8]    103ns..205ns    n=14060005   cum= 94.29%
  [ 9]    205ns..410ns    n=213278     cum= 95.36%
  [10]    410ns..820ns    n=328442     cum= 97.00%
  [11]    820ns..1.6us    n=356753     cum= 98.79%
  [12]    1.6us..3.3us    n=177258     cum= 99.67%
  [13]    3.3us..6.6us    n=11752      cum= 99.73%
  [14]    6.6us..13.1us   n=786        cum= 99.74%
  [15]   13.1us..26.2us   n=1990       cum= 99.75%
  [16]   26.2us..52.5us   n=1137       cum= 99.75%
  [17]   52.5us..105.0us  n=2235       cum= 99.76%
  [18]  105.0us..210.0us  n=4489       cum= 99.78%
  [19]  210.0us..419.9us  n=9355       cum= 99.83%
  [20]  419.9us..839.9us  n=14045      cum= 99.90%
  [21]  839.9us..1.7ms    n=9744       cum= 99.95%
  [22]    1.7ms..3.4ms    n=6733       cum= 99.98%
  [23]    3.4ms..6.7ms    n=3189       cum=100.00%
[lat-full] process n=20000000 max=3.0ms
  [ 4]      6ns..13ns     n=5610       cum=  0.03%
  [ 5]     13ns..26ns     n=19971224   cum= 99.88%
  [ 6]     26ns..51ns     n=7067       cum= 99.92%
  [ 7]     51ns..103ns    n=1390       cum= 99.93%
  [ 8]    103ns..205ns    n=177        cum= 99.93%
  [ 9]    205ns..410ns    n=1706       cum= 99.94%
  [10]    410ns..820ns    n=5873       cum= 99.97%
  [11]    820ns..1.6us    n=5611       cum= 99.99%
  [12]    1.6us..3.3us    n=720        cum=100.00%
  [13]    3.3us..6.6us    n=234        cum=100.00%
  [14]    6.6us..13.1us   n=150        cum=100.00%
  [15]   13.1us..26.2us   n=2          cum=100.00%
  [16]   26.2us..52.5us   n=2          cum=100.00%
  [19]  210.0us..419.9us  n=8          cum=100.00%
  [20]  419.9us..839.9us  n=203        cum=100.00%
  [21]  839.9us..1.7ms    n=9          cum=100.00%
  [22]    1.7ms..3.4ms    n=14         cum=100.00%
[lat-full] e2e n=20000000 max=4.9ms
  [ 7]     51ns..103ns    n=895687     cum=  4.48%
  [ 8]    103ns..205ns    n=17917709   cum= 94.07%
  [ 9]    205ns..410ns    n=229249     cum= 95.21%
  [10]    410ns..820ns    n=336257     cum= 96.89%
  [11]    820ns..1.6us    n=372823     cum= 98.76%
  [12]    1.6us..3.3us    n=181853     cum= 99.67%
  [13]    3.3us..6.6us    n=12424      cum= 99.73%
  [14]    6.6us..13.1us   n=933        cum= 99.73%
  [15]   13.1us..26.2us   n=2017       cum= 99.74%
  [16]   26.2us..52.5us   n=1144       cum= 99.75%
  [17]   52.5us..105.0us  n=2236       cum= 99.76%
  [18]  105.0us..210.0us  n=4486       cum= 99.78%
  [19]  210.0us..419.9us  n=9352       cum= 99.83%
  [20]  419.9us..839.9us  n=14078      cum= 99.90%
  [21]  839.9us..1.7ms    n=9811       cum= 99.95%
  [22]    1.7ms..3.4ms    n=6743       cum= 99.98%
  [23]    3.4ms..6.7ms    n=3198       cum=100.00%
  ```

  commit b18dc11
  ```
=== aggregated (n=20000000 drops=1) ===
[lat] transit p50=103ns p99=1.6us p999=3.3us max=934.1us n=20000000
[lat] process p50=26ns p99=51ns p999=103ns max=936.5us n=20000000
[lat] e2e p50=205ns p99=1.6us p999=3.3us max=936.6us n=20000000

=== full distribution ===
[lat-full] transit n=20000000 max=934.1us
  [ 7]     51ns..103ns    n=10166495   cum= 50.83%
  [ 8]    103ns..205ns    n=9301510    cum= 97.34%
  [ 9]    205ns..410ns    n=213150     cum= 98.41%
  [10]    410ns..820ns    n=98065      cum= 98.90%
  [11]    820ns..1.6us    n=106958     cum= 99.43%
  [12]    1.6us..3.3us    n=96769      cum= 99.91%
  [13]    3.3us..6.6us    n=15788      cum= 99.99%
  [14]    6.6us..13.1us   n=170        cum= 99.99%
  [15]   13.1us..26.3us   n=594        cum=100.00%
  [16]   26.3us..52.5us   n=20         cum=100.00%
  [17]   52.5us..105.0us  n=23         cum=100.00%
  [18]  105.0us..210.0us  n=48         cum=100.00%
  [19]  210.0us..420.1us  n=208        cum=100.00%
  [20]  420.1us..840.1us  n=165        cum=100.00%
  [21]  840.1us..1.7ms    n=37         cum=100.00%
[lat-full] process n=20000000 max=936.5us
  [ 5]     13ns..26ns     n=19766723   cum= 98.83%
  [ 6]     26ns..51ns     n=201602     cum= 99.84%
  [ 7]     51ns..103ns    n=15014      cum= 99.92%
  [ 8]    103ns..205ns    n=1556       cum= 99.92%
  [ 9]    205ns..410ns    n=13504      cum= 99.99%
  [10]    410ns..820ns    n=1197       cum=100.00%
  [11]    820ns..1.6us    n=221        cum=100.00%
  [12]    1.6us..3.3us    n=157        cum=100.00%
  [13]    3.3us..6.6us    n=24         cum=100.00%
  [15]   13.1us..26.3us   n=1          cum=100.00%
  [21]  840.1us..1.7ms    n=1          cum=100.00%
[lat-full] e2e n=20000000 max=936.6us
  [ 7]     51ns..103ns    n=164669     cum=  0.82%
  [ 8]    103ns..205ns    n=19208911   cum= 96.87%
  [ 9]    205ns..410ns    n=291383     cum= 98.32%
  [10]    410ns..820ns    n=111444     cum= 98.88%
  [11]    820ns..1.6us    n=107428     cum= 99.42%
  [12]    1.6us..3.3us    n=98598      cum= 99.91%
  [13]    3.3us..6.6us    n=16298      cum= 99.99%
  [14]    6.6us..13.1us   n=172        cum= 99.99%
  [15]   13.1us..26.3us   n=595        cum=100.00%
  [16]   26.3us..52.5us   n=20         cum=100.00%
  [17]   52.5us..105.0us  n=23         cum=100.00%
  [18]  105.0us..210.0us  n=48         cum=100.00%
  [19]  210.0us..420.1us  n=208        cum=100.00%
  [20]  420.1us..840.1us  n=165        cum=100.00%
  [21]  840.1us..1.7ms    n=38         cum=100.00%
  ```

  commit 153fec8
  ```
  === aggregated (n=20000000 drops=1) ===
[lat] transit p50=103ns p99=205ns p999=3.3us max=972.7us n=20000000
[lat] process p50=26ns p99=51ns p999=51ns max=975.1us n=20000000
[lat] e2e p50=205ns p99=205ns p999=3.3us max=975.2us n=20000000

=== full distribution ===
[lat-full] transit n=20000000 max=972.7us
  [ 7]     51ns..103ns    n=10795551   cum= 53.98%
  [ 8]    103ns..205ns    n=9058187    cum= 99.27%
  [ 9]    205ns..410ns    n=25991      cum= 99.40%
  [10]    410ns..820ns    n=37390      cum= 99.59%
  [11]    820ns..1.6us    n=45446      cum= 99.81%
  [12]    1.6us..3.3us    n=24733      cum= 99.94%
  [13]    3.3us..6.6us    n=8289       cum= 99.98%
  [14]    6.6us..13.1us   n=3362       cum= 99.99%
  [15]   13.1us..26.2us   n=673        cum=100.00%
  [16]   26.2us..52.5us   n=32         cum=100.00%
  [17]   52.5us..105.0us  n=21         cum=100.00%
  [18]  105.0us..210.0us  n=26         cum=100.00%
  [19]  210.0us..420.0us  n=82         cum=100.00%
  [20]  420.0us..840.0us  n=165        cum=100.00%
  [21]  840.0us..1.7ms    n=52         cum=100.00%
[lat-full] process n=20000000 max=975.1us
  [ 5]     13ns..26ns     n=19783358   cum= 98.92%
  [ 6]     26ns..51ns     n=198072     cum= 99.91%
  [ 7]     51ns..103ns    n=3785       cum= 99.93%
  [ 8]    103ns..205ns    n=423        cum= 99.93%
  [ 9]    205ns..410ns    n=13481      cum=100.00%
  [10]    410ns..820ns    n=839        cum=100.00%
  [11]    820ns..1.6us    n=29         cum=100.00%
  [12]    1.6us..3.3us    n=6          cum=100.00%
  [13]    3.3us..6.6us    n=4          cum=100.00%
  [14]    6.6us..13.1us   n=2          cum=100.00%
  [21]  840.0us..1.7ms    n=1          cum=100.00%
[lat-full] e2e n=20000000 max=975.2us
  [ 7]     51ns..103ns    n=940941     cum=  4.70%
  [ 8]    103ns..205ns    n=18881150   cum= 99.11%
  [ 9]    205ns..410ns    n=49319      cum= 99.36%
  [10]    410ns..820ns    n=44203      cum= 99.58%
  [11]    820ns..1.6us    n=46258      cum= 99.81%
  [12]    1.6us..3.3us    n=25316      cum= 99.94%
  [13]    3.3us..6.6us    n=8380       cum= 99.98%
  [14]    6.6us..13.1us   n=3377       cum= 99.99%
  [15]   13.1us..26.2us   n=677        cum=100.00%
  [16]   26.2us..52.5us   n=32         cum=100.00%
  [17]   52.5us..105.0us  n=21         cum=100.00%
  [18]  105.0us..210.0us  n=26         cum=100.00%
  [19]  210.0us..420.0us  n=82         cum=100.00%
  [20]  420.0us..840.0us  n=165        cum=100.00%
  [21]  840.0us..1.7ms    n=53         cum=100.00%
  ```

  commit 2cd81d8 (exec frame push in core_main, exec_main_logging on CPU 3)
  NOT under standard conditions, so not fully comparable: no fresh boot, other processes
  running (desktop session, a build just before), and the feed was not pre-read with
  `cat itch_feed/S071321-v50.txt > /dev/null`. Re-run under standard conditions to replace it.
  ```
  === aggregated (n=20000000 drops=1) ===
[lat] transit p50=205ns p99=205ns p999=3.3us max=947.6us n=20000000
[lat] process p50=26ns p99=51ns p999=103ns max=950.7us n=20000000
[lat] e2e p50=205ns p99=410ns p999=3.3us max=950.8us n=20000000

=== full distribution ===
[lat-full] transit n=20000000 max=947.6us
  [ 7]     51ns..103ns    n=4096677    cum= 20.48%
  [ 8]    103ns..205ns    n=15707715   cum= 99.02%
  [ 9]    205ns..410ns    n=37958      cum= 99.21%
  [10]    410ns..820ns    n=54473      cum= 99.48%
  [11]    820ns..1.6us    n=61746      cum= 99.79%
  [12]    1.6us..3.3us    n=28798      cum= 99.94%
  [13]    3.3us..6.6us    n=8614       cum= 99.98%
  [14]    6.6us..13.1us   n=3063       cum=100.00%
  [15]   13.1us..26.3us   n=657        cum=100.00%
  [16]   26.3us..52.5us   n=29         cum=100.00%
  [17]   52.5us..105.0us  n=16         cum=100.00%
  [18]  105.0us..210.0us  n=32         cum=100.00%
  [19]  210.0us..420.1us  n=60         cum=100.00%
  [20]  420.1us..840.1us  n=128        cum=100.00%
  [21]  840.1us..1.7ms    n=34         cum=100.00%
[lat-full] process n=20000000 max=950.7us
  [ 5]     13ns..26ns     n=19612294   cum= 98.06%
  [ 6]     26ns..51ns     n=367048     cum= 99.90%
  [ 7]     51ns..103ns    n=5653       cum= 99.92%
  [ 8]    103ns..205ns    n=369        cum= 99.93%
  [ 9]    205ns..410ns    n=12627      cum= 99.99%
  [10]    410ns..820ns    n=1830       cum=100.00%
  [11]    820ns..1.6us    n=101        cum=100.00%
  [12]    1.6us..3.3us    n=53         cum=100.00%
  [13]    3.3us..6.6us    n=13         cum=100.00%
  [14]    6.6us..13.1us   n=11         cum=100.00%
  [21]  840.1us..1.7ms    n=1          cum=100.00%
[lat-full] e2e n=20000000 max=950.8us
  [ 7]     51ns..103ns    n=221238     cum=  1.11%
  [ 8]    103ns..205ns    n=19555612   cum= 98.88%
  [ 9]    205ns..410ns    n=53193      cum= 99.15%
  [10]    410ns..820ns    n=64322      cum= 99.47%
  [11]    820ns..1.6us    n=63275      cum= 99.79%
  [12]    1.6us..3.3us    n=29544      cum= 99.94%
  [13]    3.3us..6.6us    n=8762       cum= 99.98%
  [14]    6.6us..13.1us   n=3092       cum=100.00%
  [15]   13.1us..26.3us   n=662        cum=100.00%
  [16]   26.3us..52.5us   n=29         cum=100.00%
  [17]   52.5us..105.0us  n=16         cum=100.00%
  [18]  105.0us..210.0us  n=32         cum=100.00%
  [19]  210.0us..420.1us  n=60         cum=100.00%
  [20]  420.1us..840.1us  n=128        cum=100.00%
  [21]  840.1us..1.7ms    n=35         cum=100.00%
  ```

  commit 031fa98 (plan Phase 1: MarketUpdate<10> built in place, parser bound 8192, feed drop counting)
  NOT under standard conditions, so not fully comparable: no fresh boot and other processes
  running, as for 2cd81d8 above (the feed was pre-read this time). Only 14,425 of the 20M events
  are NVDA frames, so the exec frame path is barely exercised by this benchmark.
  ```
  === aggregated (n=20000000 drops=1 frames=14425 frames_dropped=0) ===
[lat] transit p50=205ns p99=205ns p999=1.6us max=960.3us n=20000000
[lat] process p50=26ns p99=51ns p999=103ns max=963.3us n=20000000
[lat] e2e p50=205ns p99=205ns p999=1.6us max=963.5us n=20000000

=== full distribution ===
[lat-full] transit n=20000000 max=960.3us
  [ 7]     51ns..103ns    n=5996576    cum= 29.98%
  [ 8]    103ns..205ns    n=13915589   cum= 99.56%
  [ 9]    205ns..410ns    n=24719      cum= 99.68%
  [10]    410ns..820ns    n=27980      cum= 99.82%
  [11]    820ns..1.6us    n=22723      cum= 99.94%
  [12]    1.6us..3.3us    n=9100       cum= 99.98%
  [13]    3.3us..6.6us    n=2208       cum= 99.99%
  [14]    6.6us..13.1us   n=583        cum=100.00%
  [15]   13.1us..26.3us   n=220        cum=100.00%
  [16]   26.3us..52.5us   n=17         cum=100.00%
  [17]   52.5us..105.0us  n=16         cum=100.00%
  [18]  105.0us..210.0us  n=33         cum=100.00%
  [19]  210.0us..420.1us  n=66         cum=100.00%
  [20]  420.1us..840.1us  n=132        cum=100.00%
  [21]  840.1us..1.7ms    n=38         cum=100.00%
[lat-full] process n=20000000 max=963.3us
  [ 5]     13ns..26ns     n=19750525   cum= 98.75%
  [ 6]     26ns..51ns     n=222328     cum= 99.86%
  [ 7]     51ns..103ns    n=11072      cum= 99.92%
  [ 8]    103ns..205ns    n=1167       cum= 99.93%
  [ 9]    205ns..410ns    n=12546      cum= 99.99%
  [10]    410ns..820ns    n=2266       cum=100.00%
  [11]    820ns..1.6us    n=63         cum=100.00%
  [12]    1.6us..3.3us    n=22         cum=100.00%
  [13]    3.3us..6.6us    n=7          cum=100.00%
  [14]    6.6us..13.1us   n=3          cum=100.00%
  [21]  840.1us..1.7ms    n=1          cum=100.00%
[lat-full] e2e n=20000000 max=963.5us
  [ 7]     51ns..103ns    n=201787     cum=  1.01%
  [ 8]    103ns..205ns    n=19679188   cum= 99.40%
  [ 9]    205ns..410ns    n=43946      cum= 99.62%
  [10]    410ns..820ns    n=38878      cum= 99.82%
  [11]    820ns..1.6us    n=23459      cum= 99.94%
  [12]    1.6us..3.3us    n=9375       cum= 99.98%
  [13]    3.3us..6.6us    n=2251       cum= 99.99%
  [14]    6.6us..13.1us   n=592        cum=100.00%
  [15]   13.1us..26.3us   n=221        cum=100.00%
  [16]   26.3us..52.5us   n=17         cum=100.00%
  [17]   52.5us..105.0us  n=16         cum=100.00%
  [18]  105.0us..210.0us  n=33         cum=100.00%
  [19]  210.0us..420.1us  n=66         cum=100.00%
  [20]  420.1us..840.1us  n=132        cum=100.00%
  [21]  840.1us..1.7ms    n=39         cum=100.00%
  ```

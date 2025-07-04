# nanoprobe

"nanoprobe" is a high time accuracy ping-like program, based on [nanoping](https://github.com/iij/nanoping), that relies on the hardware time stamping function of the Network Interface Card (NIC).
The nanoprobe tool enables precise packet timestamping at NIC phy/mac layer, and thereby allows precise measurements of the actual network latency that is unaffected by any additional delay/jitter components form the local network stack or application.

Timestamp resolution and precision is defined by the hardware oscillator on the Ethernet controller.
For example, minimum timestamping resolution is 12.5ns on the Intel X550 Ethernet controller, because it has 80MHz oscillator for the timestamping function.

## Requirements
- Linux 4.x kernel or newer (we've tested on Ubuntu 22.04LTS)
- NIC with support for hardware timestamping (`SOF_TIMESTAMPING_{RX,TX}_HARDWARE`) and driver support for it
  - We've tested with the Intel X550-T2 Ethernet card
  - You can check support with `ethtool -T <interface>`
  - If you do not have a NIC with hardware timestamping support, you can run with the `--emulation` option to get software (at application layer) timestamps instead, but then you are likely better served by e.g. [IRTT](https://github.com/heistp/irtt)

## How to build

- run ```make``` command on the nanoprobe source directory.
- run ```make install```

## How to use

nanoprobe is a client and server application.
You have to setup the server mode nanoprobe on the target server before starting client mode nanoprobe.
When the nanoprobe server is running, the user can then start the client side nanoprobe.
Root privilege is typically required to use nanoprobe with hardware timestamping (not needed in `--emulation` mode).

Following is a typical example of nanoprobe usage.

On server host:

```
$ nanoprobe --server --interface <network interface>
```
On client host:

```
$ nanoprobe --client --interface <network interface> --delay 10000 <IP address of server host>
```

By default, the nanoprobe server and client only provide some high level statistics at the end of a session.
To get per-packet output (that can be used to calculate e.g. RTTs, one-way delay etc) use the `--log file` option

### Command line options

The same nanoprobe binary can work as either server or client, depending on the initial (mandatory) `--client` or `--server` option.
The two modes have the different command line options as below.

```console
$ nanoprobe --help
usage:
  client: nanoprobe --client --interface [nic] --count [sec] --delay [usec] --port [port] --log [logfile] --emulation --timeout [usec] --busypoll [usec] --dummy-pkt [cnt] --timer [timer-type] --ping-size [bytes] --pong-size [bytes] --probe-schedule [csv] --pong-every [n] --reverse/--duplex [host]
  server: nanoprobe --server --interface [nic] --port [port] --log [logfile] --emulation --timeout [usec] --busypoll [usec] --dummy-pkt [cnt] --probe-schedule [csv]
```

- `--client`: Run in client mode, the IPv4 address of the server has to be provided as the **final** argument.
- `--server`: Run in server mode.
- `--interface/-i <iface>`: specify interface name to send/receive ping-pong packets.
- `--port/-p <port>`: Specify the port to bind to (if server) or connect to (if client). Default port is 10666.
- `--log/-l <filename>`: Log per-packet timestamps in a CSV-format to the provided file.
- `--count/-n <sec>`: The duration of the nanoprobe measurement in seconds. Set to 0 to run forever (only supported in the "forward" test direction)
- `--delay/-d <usec>`: The target time in microseconds between sending ping packets.
- `--emulation/-e`: Use software timestamps from the nanoprobe process instead of hardware timestamps supplied by the NIC
- `--timeout/-t <usec>`: Set the timeout value in microseconds for how long to wait for a ping or pong packet before failing. Sets the `SO_RCVTIMEO` socket option on the ping/pong socket. Default is 5 seconds.
- `--busypoll/-b`: Set `SO_BUSY_POLL` socket option for ping/pong socket (see man socket(7) for details).
- `--dummy-pkt/-x <n>`: Send n additional packets during each ping-pong transaction. This option produces broader bandwidth for the measurements stream. Inherited from nanoping but not really tested with nanoprobe.
- `--timer/-T <sleep/busy>`: Determines how to wait for the required time between packets to achieve the target delay. The sleep option tries to sleep until the next packet needs to be sent, but this tends to be quite inaccurate at sub-millisecond timescales. The `busy` alternative instead busy loops until the next packet needs to be sent, which achieves much greater accuracy (typically within one microsecond) but at the cost of burning CPU cycles. Defaults to busy if delay <= 1ms, otherwise sleep.
- `--ping-size/-s <bytes>`: The size of the ping packets (including the IP-header, but not the Ethernet header). Defaults to the smallest possible size (currently 44 bytes).
- `--pong-size/-o <bytes>`: The size of the ping packets (including the IP-header, but not the Ethernet header). Defaults to the smallest possible size (currently 44 bytes).
- `--probe-schedule/-S <csv-file>`: Provide a schedule for the ping packets in a CSV with the delay and ping-size for each packet. The CSV format is `<delay>,<ping-size>` (with delay and ping-size specified as for the corresponding options) with one line per packet, no header. The test will run until each packet in the schedule has been sent. This option overrides any supplied delay, ping-size and count options. In order for the server to be able to follow a schedule (in the reverse and duplex modes) the server must be started with this option, the client will not share the schedule with the server.
- `--pong-every/-y <n>`: Only pong (reply to) every n:th ping message. The value 0 will disable ponging altogether (NOTE: this may cause the application to timeout depending on the test duration and the supplied `timeout` value). Default is to pong every ping.
- `--reverse/-R`: Instead of the nanoprobe client pinging the server, request that the server pings the client. This can be useful if the client is behind NAT, so that the server and clients cannot simply swap places. This is mutually exclusive with the `duplex` option.
- `--duplex/-D`: Instead of the nanoprobe server ponging the pings from the client, make it independently (although starting on the reception of the first ping from the client) send its own pings to the client. This is mutually exclusive with the `reverse` option.




## Time stamp points

```
        client          server
          |    ping       |
        t0|-------------->|t1
          |               |
          |    pong       |
        t3|<--------------|t2
          |               |
          v               v
```

Each ping-pong transaction has 4 timestamps, and these are enough to calculate RTT (Round Trip Time) between client and server. RTT is calculated by (t3-t0) - (t2-t1). If the client and server have synchronized clocks it's also possible to calculate the OWD as t1 - t0 and t3 - t2.

The sketch above assumes the default "forward" test, where the client pings the server. For the reverse mode the server will ping the client, so t0 will then be the time the server sends the ping. In other words, t0 is always the time of ping transmission, t1 the time of ping reception, t2 the time of pong transmission, and t3 the time of pong reception, regardless of which side is sending the pings and pongs.

In the duplex mode there are no pongs, so no t2 or t3 timestamps. To distinguish the timestamps of the pings sent by the client from the pings sent by the server, the timestamps may be suffixed by `cs` (client to server) or `sc` (server to client) to indicate the direction of the packet.

## Accuracy of nanoprobe timestamps

Nanoprobe uses the timestamp function of the Ethernet controller hardware.
Hardware timestamping is driven by the oscillator on the Ethernet controller, therefore the accuracy of the nanoprobe measurement depends on the acuracy of it.

For example, Intel X550 ethernet controller has 80MHz oscillator for the time stamp hardware (*1). The resolution is a 12.5ns tick. Nanoprobe can record all timestamps in the accuracy.
However, each individual internal oscillator has its unique characteristics,
Usually, a little clock difference exists in some PPMs (up to 50ppm).
So without external reference clock to calibrate the internal oscillator, the clock diffrence between client and server ethernet controllers generate some errors to prevent enough accurate OWD (one-way delay) calculation.

(*1) [Intel Ethernet controller X550 Datasheet](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/ethernet-x550-datasheet.pdf)  7.7 Time SYNC (IEEE1588 and 802.1AS)


## Log file format

With the ```--log``` option, nanoprobe records all local packet timestamps in a CSV, i.e. timestamps for the packets that the client or server receives or transmits themselves.
To get all 4 timestamps of a ping-pong transaction, it's therefore necessary to collect the logs from both the server and the client.

The general format is `seq,timestamp-idx,timestamp,size`, where seq is the ping/pong sequence number, timestamp-idx is the t0-t3 identifier from the [previous section](#time-stamp-points) denoting the type of timestamp, the timestamp is unix-like timestamp (i.e. time in seconds since the epoch of the hardware clock, with the fractional part having nanosecond resolution), and size is the packet size in bytes (including the IP header).

Example client log:
```csv
seq,timestamp-idx,timestamp,size
1,t0,1750785880.108711082,1500
1,t3,1750785880.108856872,44
2,t0,1750785880.208848716,1500
2,t3,1750785880.208985752,44
3,t0,1750785880.309014078,1500
3,t3,1750785880.309151451,44
4,t0,1750785880.409012596,1500
4,t3,1750785880.409150208,44
5,t0,1750785880.508925389,1500
5,t3,1750785880.509061837,44
...
```

Example server log:
```csv
seq,timestamp-idx,timestamp,size
1,t1,1750785880.108757199,1500
1,t2,1750785880.108821219,44
2,t1,1750785880.208893991,1500
2,t2,1750785880.208953081,44
3,t1,1750785880.309060320,1500
3,t2,1750785880.309118229,44
4,t1,1750785880.409057529,1500
4,t2,1750785880.409117519,44
5,t1,1750785880.508970814,1500
5,t2,1750785880.509030545,44
...
```

For the duplex mode, where the timestamp-idx is a bit different as explained in the [previous section](#time-stamp-points), the client log might look like:
```csv
seq,timestamp-idx,timestamp,size
1,t0cs,1750786039.119817369,44
1,t1sc,1750786039.120157983,44
2,t0cs,1750786039.219992840,44
2,t1sc,1750786039.220227892,44
3,t0cs,1750786039.320095265,44
3,t1sc,1750786039.320202667,44
4,t1sc,1750786039.420159023,44
4,t0cs,1750786039.420114931,44
5,t1sc,1750786039.520227558,44
5,t0cs,1750786039.520130364,44
...
```

And the server log:
```csv
seq,timestamp-idx,timestamp,size
1,t1cs,1750786039.119919386,44
1,t0sc,1750786039.120071407,44
2,t1cs,1750786039.220087266,44
2,t0sc,1750786039.220149850,44
3,t1cs,1750786039.320188952,44
3,t0sc,1750786039.320174470,44
4,t1cs,1750786039.420159057,44
4,t0sc,1750786039.420114930,44
5,t1cs,1750786039.520227565,44
5,t0sc,1750786039.520160273,44
...
```

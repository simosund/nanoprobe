# Probe schedules

This directory contains pre-made probe schedules that can be used with
nanoprobe's `-S`/`--probe-schedule` option.

### Description of probe schedules
- **250502_Q_para1_RateSweep_1500_only.csv:** Probe schedule used for
  testing Starlink's queuing configuration, used for the paper
  "Characterizing the Configuration of Starlink Queuing" accepted for
  presentation at AMC Internet Measurement Conference
  (IMC) 2026. Contains 120 different traffic bursts, testing all
  combinations of 15 different sending rates (48, 96, 150, 200, 240,
  279, 300, 324, 353, 375, 400, 429, 462, 500, and 545 Mbps) and 8 burst
  sizes (200, 500, 1000, 2000, 3000, 4000, 5000, and 6000
  packets). The complete schedule consists of 325500 packets and
  should take around 4min20sec to run.

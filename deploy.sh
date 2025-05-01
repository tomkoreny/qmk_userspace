#!/usr/bin/env bash
qmk compile
sleep 3
sudo mkdir /tmp/kb
sudo mount /dev/sdd1 /tmp/kb
sudo cp splitkb_aurora_corne_rev1_miryoku_553.uf2 /tmp/kb/fw.uf2

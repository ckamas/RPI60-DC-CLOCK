#!/bin/bash
set -x

gcc clock.c -o clock -lwiringPi

set +x
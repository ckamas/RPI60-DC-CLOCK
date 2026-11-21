#!/bin/bash
set -x

gcc clock.c -o clock -llgpio

set +x
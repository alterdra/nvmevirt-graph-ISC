#!/bin/bash
if lsmod | grep -q nvmev0; then
    sudo rmmod nvmev0
fi
if lsmod | grep -q nvmev1; then
    sudo rmmod nvmev1
fi
if lsmod | grep -q nvmev2; then
    sudo rmmod nvmev2
fi
if lsmod | grep -q nvmev3; then
    sudo rmmod nvmev3
fi
if lsmod | grep -q nvmev4; then
    sudo rmmod nvmev4
fi
if lsmod | grep -q nvmev5; then
    sudo rmmod nvmev5
fi
if lsmod | grep -q nvmev6; then
    sudo rmmod nvmev6
fi
if lsmod | grep -q nvmev7; then
    sudo rmmod nvmev7
fi
if lsmod | grep -q hmb; then
    sudo rmmod hmb
fi
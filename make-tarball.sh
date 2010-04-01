#!/bin/bash

VF=version.txt

if [ -r $VF -a -f $VF -a -s $VF ]; then
    set $(cat version.txt) NONE NULL
    DIR=$1
    VER=$2
    
    cd ..
    if [ -d $DIR ]; then
	mv $DIR $DIR-$VER
	tar czvf $DIR-$VER.tar.gz $DIR-$VER
	mv $DIR-$VER $DIR
    fi
fi



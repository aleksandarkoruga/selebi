// without mul and add.
BufPlaySelebi : UGen {
    *ar { arg bufNum, speed = 1, pitch = 1, position = 0, trigPos = 0, posChangeAlgo = 0;
        ^this.multiNew('audio', bufNum, speed, pitch, position, trigPos, posChangeAlgo);
    }
}

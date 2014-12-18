Multirec
========

*The poor man's multichannel audio recorder*

DISCLAIMER!! 
-------------

This is an old program that i wrote back in 2009. I put it here on GitHub just because it was sad to have it sitting there in my hard drive, and maybe it could be useful as an example or inspiration for someone else.  
Nowadays though, the same functionality implemented here (and much more!) is provided by JACK via the alsa_in client.  

So, **please**, take a look at JACK before using or forking this software!! Here are some links:

* [https://github.com/jackaudio/jackaudio.github.com/wiki/WalkThrough_User_AlsaInOut](https://github.com/jackaudio/jackaudio.github.com/wiki/WalkThrough_User_AlsaInOut)
* [http://manpages.ubuntu.com/manpages/raring/man1/alsa_in.1.html](http://manpages.ubuntu.com/manpages/raring/man1/alsa_in.1.html)
* [http://www.jackaudio.org/faq/multiple_devices.html](http://www.jackaudio.org/faq/multiple_devices.html)


What is multirec?
-----------------
This program makes it possible to record audio simultaneously from multiple consumer-grade stereo sound cards, and automagically keeps the recorded data streams in synch, as if they were captured by a real multi-channel device.
In other words, you can make your own multitrack recorder out of junk computer parts.

__BUT,__ you pay a very high price for this!! That is: audio quality. This program uses [libsamplerate](http://www.mega-nerd.com/SRC/index.html) to "stretch" the recorded audio streams in order to keep them in sync with each other; libsamplerate is __totally awesome__, but, as any sample rate converter, invariably introduces some artifacts, and makes you lose some bandwidth.

So, if you'd like to try out this software, please keep in mind that its results are far from those of a real multitrack device. With the price that those M-Audios and Tascams have reached these days, there's no real reason for a solution like this. I wrote it just for the heck of it, and... you know, sometimes you just DIY or die ;-)

That said, not being an audio expert at all, i think the results are at least decent if you're not too much of a hi-fi guy, and i'm pretty satisfied.

Actually, the first version of the program ran on a Pentium 166MHz with 64MB of RAM, driving 2 soundcards (4 channels in total). That configuration was used successfully back in May 2009 to record a whole album : "Stolen Blues" by R'n'R Terrorists, a kick-ass italian blues/punk band. They have a song from that album on YouTube, check it out: [http://www.youtube.com/watch?v=EpxBFZX1RfU](http://www.youtube.com/watch?v=EpxBFZX1RfU)  
(...aehm, note that the voice channel was distorted on purpose :-D)


Building
--------

Simply type:

    make

and it will create an executable called `multirec`. That's all.

Multirec depends on the following libraries that you need to have installed on your system:

* libasound (part of alsa, you should already have it...)
* libpthread (part of glibc, you should already have it...)
* [libsamplerate](http://www.mega-nerd.com/SRC/index.html)
* [libncurses](http://www.gnu.org/software/ncurses/)
* [libsndfile](http://www.mega-nerd.com/libsndfile/)

These are all mainstream libraries, so they'll be easily available via you distro's package manager. 


Usage
-----
The usage of this software could not be simpler, and its functionality could not be more limited:

    multirec <trackname>

Where `trackname` is the name of the track that is going to be recorded.

Before launching the program, make sure you have the configuration file `multirec.rc` under the current directory. This file contains the soundcards configuration data, one line for each soundcard. Please refer to the comments in the provided .rc file for details.

The program has a very trivial ncurses interface, showing only some basic VU meters, one per channel. As soon as the program starts, it is in the "monitoring" state: audio is captured from the devices and shown on the VU meters, but no data is saved on disk. You can now adjust your volumes.

Once you are ready to start recording, press the `r` key. Now you should see a red `REC` label at the bottom, and data gets written to disk.

When you're done and you want to stop recording, press `q`. Then, if you _really_ mean to stop recording, press `y`. The program does a bit of cleanup and exits. Read on to discover where and how data was saved...

Audio format
------------

Data is saved as mono wave (.wav) files, one file per channel. For example, if you have 4 sound cards, the program will spit out 8 .wav files.

Samples are 48kHz, signed 16bit integer, little endian. This configuration is hardcoded, mostly because i'm too slack, and anyway most consumer soundcards seem to support it.

The file name pattern is `trackname-NN/c.wav`, where:

 * `trackname` is the name of the track you are recording. This will be the base name of the subdirectories where multirec will store your recordings.
 * `NN` is the attempt number, or a 2-digit counter that increments by 1 each time you record the same track. This is very useful when you record a band playing the same track over and over trying to achieve perfection :-)
 * `c` is the channel id, or an alphabetic counter starting from 'a' which identifies the channel.

So for example, suppose you are using 2 soundcards, and you want to record a track named "foo" :

    $ multirec foo

When the recording starts, the program looks for a subdirectory whose name starts with "foo-" under the current dir. It doesn't find one, so it creates the subdir for the first attempt, named "foo-01/". Finally, it creates the audio files:

    foo-01/a.wav
    foo-01/b.wav
    foo-01/c.wav
    foo-01/d.wav

Then you stop recording and the program exits. The performance was not just right, so you want to record the same song again. Easy, just launch the same command as before:

    $ multirec foo

Now, the program finds the subdirectory, and sees that the last attempt count is "01", so it advances the counter, and creates :

    foo-02/a.wav
    foo-02/b.wav
    foo-02/c.wav
    foo-02/d.wav

And so on, and so on, all the way to 99.

When you want to record a different song, just specify a different name, and everything starts again with a new basename and an attempt count of "01".

...and that's all it does. Have fun! :-)

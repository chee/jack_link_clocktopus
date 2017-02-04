# jack_link 

  **jack_link** is a [JACK](http://jackaudio.org) transport timebase
  prototype bridge to [Ableton Link](https://www.ableton.com/en/link/).

  Upstream author: Rui Nuno Capela <rncbc@rncbc.org>.


## Prerequisites

   **jack_link** software prerequisites for building are a C++11 compiler
   (_g++_), the [JACK](http://jackaudio.org) client C libraries and headers
   (_libjack-devel_) and the [Asio C++ Library](http://think-async.com/Asio/)
   for cross-platform network and low-level I/O (_asio-devel_).


## Building

   **jack_link** relies on [link](https://github.com/Ableton/link) as a Git 
   submodule, so after the main [jack_link](https://github.com/rncbc/jack_link)
   repository is cloned, one needs to setup the working tree as follows:

     git clone https://github.com/rncbc/jack_link
     cd jack_link
     git submodule update --init

     make

   Then just run it:

     ./jack_link

   To quit, enter `quit` on the `jack_link>` prompt:

     jack_link> quit

   Enjoy.


## License

   **jack_link** is free, open-source [Linux Audio](http://linuxaudio.org)
   software, distributed under the terms of the GNU General Public License
   ([GPL](http://www.gnu.org/copyleft/gpl.html)) version 2 or later.


## Copyright

   Copyright (C) 2017, rncbc aka Rui Nuno Capela. All rights reserved.

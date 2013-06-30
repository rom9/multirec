/*
 * Copyright 2010 Alberto Romei
 * 
 * This file is part of Multirec.
 *
 * Multirec is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Multirec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Multirec.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ncurses.h>
#include <signal.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#include "multirec.h"
#include "main.h"

int exitRequested = 0;
int stopRequested = 0;

int confirmStop = 0;

short oldLevels[4][2] = {{10,10},{10,10},{10,10},{10,10}};
short levels[4][2] = {{0,0},{0,0},{0,0},{0,0}};


void cmdStartRec() {
	attron(A_BOLD | A_BLINK | COLOR_PAIR(COLOR_RED));
	int y = getmaxy(stdscr);
	//~ char buf[100];
	//~ sprintf(buf, "%d, %d", y, x);
	//~ mvaddstr(10, 10,buf);
	mvaddstr(y-1, 10, "REC");
	attroff(A_BOLD | A_BLINK | COLOR_PAIR(COLOR_RED));
	
	startRecording();
}


void cmdStopRec() {
	attron(A_BOLD);
	mvaddstr(10, 10, "Really exit (y/n) ? ");
	attroff(A_BOLD);

	confirmStop = 1;
}


inline void plotLevel(short lev, short devNum, short chNum) {
	int i;
	short xpos = 3 + ((devNum*6) + (chNum*3));
	short ypos = 1-(lev/2);
	for(i=oldLevels[devNum][chNum]; i<ypos; i++) {
		mvaddch(i , xpos, ' ');
	}

	for(i=ypos; i<=oldLevels[devNum][chNum]; i++) {
		chtype c = (i==ypos && lev % 2 != 0) ? ACS_S7 : ACS_CKBOARD;
		if(i<=2) {
			attron(COLOR_PAIR(COLOR_RED));
			mvaddch(i , xpos, c);
			attroff(COLOR_PAIR(COLOR_RED));
		} else if(i<=5) {
			attron(COLOR_PAIR(COLOR_GREEN));
			mvaddch(i , xpos, c);
			attroff(COLOR_PAIR(COLOR_GREEN));
		} else {
			mvaddch(i , xpos, c);
		}
	}


	oldLevels[devNum][chNum] = ypos;
}


void monitor() {
	int i;
	for(i=0; i<devCount; i++) {
		// Plot DEV 0, CH 0 level
		short lev = 10*log10(devices[i]->peaks[0] / 32768.0);
		plotLevel(lev>-18 ? lev : -18, i, 0);

		// Plot DEV 0, CH 1 level
		lev = 10*log10(devices[i]->peaks[1] / 32768.0);
		plotLevel(lev>-18 ? lev : -18, i, 1);
	}
	refresh();
}


int main(int argc, char *argv[]) {

	// Expects a track name as argument, which will become the output dir
	// where to save this recording session's tracks.
	if(argc<2) {
		printf("Output dir not specified!\n");
		return -1;
	}
	


    signal(SIGINT, finish);      /* arrange interrupts to terminate */

    initscr();      /* initialize the curses library */
    keypad(stdscr, TRUE);  /* enable keyboard mapping */
    nonl();         /* tell curses not to do NL->CR/NL on output */
    noecho();       /* don't echo input */
    cbreak();       /* take input chars one at a time, no wait for \n */

    nodelay(stdscr, TRUE); /* getch() will be non-blocking */
    curs_set(0);

    if (has_colors()) {
        start_color();
        init_pair(COLOR_BLACK, COLOR_BLACK, COLOR_BLACK);
        init_pair(COLOR_GREEN, COLOR_GREEN, COLOR_BLACK);
        init_pair(COLOR_RED, COLOR_RED, COLOR_BLACK);
        init_pair(COLOR_CYAN, COLOR_CYAN, COLOR_BLACK);
        init_pair(COLOR_WHITE, COLOR_WHITE, COLOR_BLACK);
        init_pair(COLOR_MAGENTA, COLOR_MAGENTA, COLOR_BLACK);
        init_pair(COLOR_BLUE, COLOR_BLUE, COLOR_BLACK);
        init_pair(COLOR_YELLOW, COLOR_YELLOW, COLOR_BLACK);
    }

	init(argv[1]);


    for (;;) {
		usleep(10000);
		int c = getch();     /* refresh, accept single keystroke of input */

		monitor();

		// Got out of the last callback run (after a stop request). Stop
		// looping and end the prog.
		if(exitRequested)
			break;


		switch (c) {
		case 'q':
			cmdStopRec();
			break;
		case 'r':
			cmdStartRec();
			break;
		case 'm':
			monitor();
			break;
		case 'y':
			if (confirmStop) {
				// Stop the recording hardware and finalize the files...
				stopRecording();
				// ...then, exit the curses loop and end the program
				exitRequested = 1;
			}
			break;
		}
        
    }

    finish(0);
    return 0;
}


void finish(int sig) {
    endwin();
	
    exit(sig);
}

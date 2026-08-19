10 REM First we draw the clock face
20 FOR n=1 TO 12
30 PRINT AT 10-10*COS (n/6*PI),16+10*SIN (n/6*PI);n
40 NEXT n
41 REM The numbers PEEK 23674, PEEK 23673 and PEEK 23672 are held inside
42 REM the computer and used by the system for counting in
43 REM 50ths of a second. Each is between 0 and 255, and they
44 REM increase through all the numbers from 0 to 255;
45 REM after 255 they drop straight back to 0.
46 REM The one that increases most often is PEEK 23672. Every 1/50 second
47 REM it increases by 1. When it is at 255, the next increase takes it to 0,
48 REM and at the same time it nudges PEEK 23673 by up to 1. When (every 256/50 seconds)
49 REM PEEK 23673 is nudged from 255 to 0, it in turn nudges PEEK 23674 up by 1.
50 DEF FN t()=lNT (65536*PEEK 23674+256*PEEK 23673+ PEEK 23672)/50): REM number of seconds since start
100 REM Now we start the clock
110 LET t1=FN t()
120 LET a=t1/30*PI: REM a is the angle of the second hand in radians
130 LET sx=72*SIN a: LET sy=72*COS a
140 PLOT 131,91: DRAW OVER 1;sx,sy: REM draw hand
200 LET t=FN t()
210 IF t<=t1 THEN GO TO 200: REM wait until time for next hand
220 PLOT 131,91: DRAW OVER 1;sx,sy: REM rub out old hand
230 LET t1=t: GO TO 120

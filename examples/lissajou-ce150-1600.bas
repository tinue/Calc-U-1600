10 REM LISSAJOU (C) Martin Erzberger, 2026
20 "L" CLEAR : WAIT 0
30 RADIAN: CSIZE 2
40 LPRINT "Lissajous Figure"
50 COLOR 3 : GRAPH : GLCURSOR (108, -120) : SORGN
60 A = 100 : B = 100 : F = 3 : G = 2 : D = PI / 2 : L = -5
70 FOR I = 0 TO 100
80 T = I * PI / 50
90 X = A * SIN (F * T + D)
100 Y = B * SIN (G * T)
110 IF I = 0 THEN GLCURSOR (X, Y) : GOTO "SKIP"
120 LLINE -(X, Y)
130 "SKIP" P = I
140 IF P >= L + 5 THEN PRINT "Progress:"; P; "%" : L = P
150 NEXT I
160 GLCURSOR (0, -120)
170 TEXT : PRINT "Complete"
180 WAIT : END

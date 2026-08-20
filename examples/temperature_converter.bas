10 PRINT "TEMPERATURE CONVERTER"
20 PRINT ,,"Press ""F"" to convert from","Fahrenheit or ""C"" to convert","from Celsius"
30 IF INKEY$="f" THEN GO TO 90
40 IF INKEY$<>"c" THEN GO TO 30
50 PRINT ,,"Enter Celsius degrees"
60 INPUT c
70 LET f=(c*9/5)+32
80 GO TO 120
90 PRINT ,,"Enter Fahrenheit degrees"
100 INPUT f
110 LET c=(f-32)*5/9
120 PRINT ,,"RESULTS"
130 PRINT ,,f;" degrees Fahrenheit"
140 PRINT ,,c;" degrees Celsius"

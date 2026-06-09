# DSK structure            		/ MEMORY  MAP / RAM  TYPE
# boot0:    	T00/S00				$0800		 MAIN
# FLOAD:		T00/S01-T00/S05 	$FC00		RAMCARD
# MAIN:  		T01/S00-T01/S0F		$D000		RAMCARD
# PLAYERTS+PT3: T02/S00-T05/SXX		$8000		 MAIN
# HGRs:			T06/S00-T09/S0F		$2000		 MAIN
# SUBROUTINE:	T0A/S00-T0A/SXX		$6000		 MAIN	

player: boot.b fload.b pts.b sub.b main.b

boot.b: boot.a
    @echo "boot part"
    %A2SDK%\BIN\acme -f plain -o boot.b boot.a
    %A2SDK%\BIN\dw.py dsk\test.dsk boot.b 0 0 + p

fload.b: fload.a
    @echo "fload part"
    %A2SDK%\BIN\acme -f plain -o fload.b fload.a
    %A2SDK%\BIN\dw.py dsk\test.dsk fload.b 0 1 + p

pts.b: pts.a
    @echo "player TS part"
    %A2SDK%\BIN\acme -f plain -o pts.b pts.a
    %A2SDK%\BIN\dw.py dsk\test.dsk pts.b 2 0 + D

sub.b: sub.a lbl_pts.txt
    @echo "Subroutines part"
    %A2SDK%\BIN\acme -f plain -o sub.b sub.a
    %A2SDK%\BIN\dw.py dsk\test.dsk sub.b A 0 + D

main.b: main.a pts.b sub.b lores.bin
    @echo "main part"
    %A2SDK%\BIN\acme -f plain -o main.b main.a
    %A2SDK%\BIN\dw.py dsk\test.dsk main.b 1 0 + D

clean:
	@echo "cleaning..."
	del boot.b
	del fload.b
	del main.b
	del pts.b
	del sub.b
	del lbl_pts.txt
	del lbl_sub.txt
    
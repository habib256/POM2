# DSK/MEMORY structure     / MEMORY  MAP / RAM  TYPE / DeComp.
# boot0:     T00/S00			$0800		 MAIN
# FLOAD:	 T00/S01-T00/S05 	$FC00		RAMCARD
# TITLE PIC: T01/S00-T02/S0F	$2000        MAIN
# MAIN:  	 T03/S00-T0X/SXX	$D000		RAMCARD
# ROUTINE:   T05/S00-T06/SXX	$4000(c)	 MAIN	   $6000(M) 
# LORES:     T07/S00-TXX/SXX    $1000(c)      AUX      LO:PAGE1/PAGE2(M)
# HIRES:	 T08/S00-TXX/SXX	$2000(c)      AUX	   HI:PAGE1/PAGE2(M)

player: boot.b fload.b main.b routine.b

boot.b: boot.a
    @echo "boot part"
    %A2SDK%\BIN\acme -f plain -o boot.b boot.a
    %A2SDK%\BIN\dw.py dsk\test.dsk boot.b 0 0 + p

fload.b: fload.a
    @echo "fload part"
    %A2SDK%\BIN\acme -f plain -o fload.b fload.a
    %A2SDK%\BIN\dw.py dsk\test.dsk fload.b 0 1 + p

main.b: main.a chp7.a player_CHIPNSFX.a routine.b
    @echo "main part"
    %A2SDK%\BIN\acme -f plain -o main.b main.a
    %A2SDK%\BIN\dw.py dsk\test.dsk main.b 3 0 + D

routine.b: routine.a
	@echo "routine part"
    %A2SDK%\BIN\acme -f plain -o routine.b routine.a
	%A2SDK%\BIN\LZ4 -2 routine.b
    %A2SDK%\BIN\dw.py dsk\test.dsk routine.b.lz4 5 0 + D

clean:
	@echo "cleaning..."
	del boot.b
	del fload.b
	del main.b
	del routine.b
	del routine.b.lz4

    
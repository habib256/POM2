# DSK structure            		/ MEMORY  MAP / RAM TYPE / COMP / DEST
# boot0:    	T00/S00				$0800		 MAIN
# FLOAD:		T00/S01-T00/S0x 	$FC00		RAMCARD
# MAIN:  		T01/S00-Txx/Sxx		$D000		RAMCARD		
# DRUM:			T04/S00-T04/S00		$8000		  AUX       *	 (->$1000A)		 
# MUSIC:		T05/S00-T06/SXX		$8000		  AUX		*  	 (->$2000A)
# GFX:			T07/S00-T0A/S0F		$2000		  MAIN

player: boot.b fload.b drum.b main.b ZIC GFX

boot.b: include.a boot.a
    @echo "boot part"
    %A2SDK%\BIN\acme -f plain -o boot.b boot.a
    %A2SDK%\BIN\dw.py dsk\test.dsk boot.b 0 0 + p

fload.b: include.a fload.a
    @echo "fload decomp part"
    %A2SDK%\BIN\acme -f plain -o fload.b fload.a
    %A2SDK%\BIN\dw.py dsk\test.dsk fload.b 0 1 + p

drum.b: drum.a
	@echo "fload decomp part"
    %A2SDK%\BIN\acme -f plain -o drum.b drum.a
	%A2SDK%\BIN\LZ4.exe -2 drum.b
    %A2SDK%\BIN\dw.py dsk\test.dsk drum.b.lz4 4 0 + D

main.b: include.a drum.a main.a
    @echo "main part"
    %A2SDK%\BIN\acme -f plain -o main.b main.a
    %A2SDK%\BIN\dw.py dsk\test.dsk main.b 1 0 + D

ZIC: music\ZICDECOMP.lz4
	@echo "Music part"
    %A2SDK%\BIN\dw.py dsk\test.dsk music\ZICDECOMP.lz4 5 0 + D

GFX: gfx\PI.1 gfx\PI.2
	@echo "Gfx part"
    %A2SDK%\BIN\dw.py dsk\test.dsk gfx\PI.1 7 0 + D
	%A2SDK%\BIN\dw.py dsk\test.dsk gfx\PI.2 9 0 + D

clean:
	@echo "cleaning..."
	del boot.b
	del fload.b
	del drum.b
	del main.b
    
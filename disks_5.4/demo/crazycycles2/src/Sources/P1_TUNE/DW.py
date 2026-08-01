# -*- coding:cp437 -*-
# WARNIG : encodage pour Console DOS sous Windows uniquement !!!

# Direct Write (c) www.ctrl-pomme-reset.fr
# version 0.41 (20.05.2014)

# Ecrit un fichier binaire directement dans une image disk (DSK) Apple II
# Entr‚es (utilisateur ou ligne de commande) :
# - fichier image DSK
# - fichier binaire
# - secteur de d‚part (en Hexa SANS 0x)
# - piste de d‚part (en Hexa SANS 0x)
# - sens ‚criture (secteur croissant/d‚croissant) (+ ou - )
# - Interleaving (D)os/(P)hysical/(F)ast Load
# Sortie :
# - fichier image DSK modifi‚ (pas de sauvegarde)

# Ex : Ligne de commande : dw.py name.dsk binary 0 1 + d
# = ‚criture de binary dans name.dsk … partir du secteur 1, piste 0, sens croissant, interleaving dos

import sys
import struct

if __name__ == '__main__':

    print
    print("Direct Write v0.41 - 2014")
    print

    if len(sys.argv) < 7:
        nameDSK = raw_input("Nom du de l'image disk : ")
	nameBinary = raw_input("Nom du fichier binaire : ")
	track = int(raw_input("Piste (Hexa) : "),16)
	sector = int(raw_input("Secteur (Hexa) : "),16)
	sens = raw_input("Sens ‚criture (+/-) : ")
	interleaving = raw_input("Interleaving (D/P/F) : ")
    else:    
	nameDSK = sys.argv[1]
	nameBinary = sys.argv[2]
	track = int(sys.argv[3],16)
	sector = int(sys.argv[4],16)
	sens = sys.argv[5]
	interleaving = sys.argv[6]

    if (interleaving == "p"):                   # physical interleaving (par rapport … un .dsk/.do)
        inter = [0x00,0x07,0x0E,0x06,0x0D,0x05,0x0C,0x04,0x0B,0x03,0x0A,0x02,0x09,0x01,0x08,0x0F]
    elif (interleaving == "f"):                 # fast load interleaving
        inter = [0x00,0x0E,0x0D,0x0C,0x0B,0x0A,0x09,0x08,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF]
    else:                                       # dos interleaving (default pour .dsk/.do)
        inter = [0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F]

    fbin = open(nameBinary,"rb")
    load = fbin.read()                          # lecture du fichier binaire
    lenBin = len(load)                          # on r‚cupŠre la taille du binaire
    
    modBin = bytearray(load)                    #
    reste = lenBin%256
    div = lenBin/256
    if (reste) != 0:                            # il y a un reste ? (donc pas un multiple de 256)
            nbSector = div+1                    # on calcule le nb de secteurs … ‚crire
            i = 0
            while i<(256*(div+1))-lenBin:       # on complŠte avec des zero pour obtenir un multplie de 256
                modBin.append(0)
                i+=1
    else:
            nbSector = div                      # si multiple de 256, nbSector est directement le r‚sultat de la div.
            
    lenmodBin = len(modBin)                     # calcul de la nouvelle taille de liste contenant les octets … ins‚rer
    offset = track*0x1000+sector*0x100          # calcul offset dans le fichier DSK pour l'‚criture
    fDSK = open(nameDSK,"rb+")                  # ouverture fichier DSK (lecture + modification)
    record = fDSK.read()                        # lecture complŠte
    if len(record) != 143360:                   # v‚rification taille standard d'un fichier DSK
        print ("erreur avec le fichier DSK")
    else :
        bufDSK = struct.Struct("<143360B")      # structure fichier DSK (143360 x 1 byte)
        outDSK = bufDSK.unpack(record)          # on unpack le fichier DSK vers la structure d‚finie
        modifiedDSK = bytearray(outDSK)         # on cr‚e une bytearray … partir du contenu pour pouvoir la modifier
        if (sens == "+"):
        # sens incr‚mental pour copier le bin dans le DSK
            t = track
            s = sector
            j = 0
            k = 0
            while j<nbSector:
                s1 = inter[s]                   # on r‚cupŠre le secteur correspondant … l'interleaving
                if s1==0xFF:
                    break                       # si mauvais secteur, on sort (Fast Load Only)
                dest = t*0x1000+s1*0x100        # calcul offset dans DSK du secteur … ‚crire
                i = 0                           # premier byte secteur en cours
                while i<256:                    # boucle ‚criture 1 secteur !
                    modifiedDSK[dest+i] = modBin[k]
                    i +=1
                    k +=1
                s +=1                           # secteur suivant
                if s>0x0F:                      # en bout de piste ?
                    s = 0                       # secteur remis … 0
                    t +=1                       # et piste suivante
                j +=1                           # nb secteur ‚crit + 1
            
        elif (sens == "-"):
        # sens d‚cr‚mental
            t = track
            s = sector
            j = 0
            k = 0
            while j<nbSector:                   # boucle 1 - nb de secteurs … ‚crire
                s1 = inter[s]                   # on r‚cupŠre le secteur correspondant … l'interleaving
                if s1==0xFF:
                    break                       # si mauvais secteur, on sort (Fast Load only)
                dest = t*0x1000+s1*0x100        # calcul offset secteur … ‚crire
                i = 0                           # premier byte secteur en cours
                while i<256:                    # boucle 2 - nb d'octets … ‚crire par secteur (256)
                    modifiedDSK[dest+i] = modBin[k]   # on copie chaque byte
                    i +=1                       # on incr‚mente de 1 (byte suivant dans le secteur)
                    k +=1                       # source (bin) + 1
                s -=1                           # secteur pr‚c‚dent
                if s<0:                         # au d‚but de piste ?
                    s = 0xF                     # on saute alors au dernier secteur de la piste suivante
                    t -=1                       # on d‚cr‚mente piste
                j +=1                           # nb secteur ‚crit + 1
                

        print 
        print "Ecriture de",nbSector,"secteurs (",hex(lenmodBin),"octets ) depuis :"
        print "Secteur",sector
        print "Piste",track
        print "Sens :",
        if (sens == "+"):
            print "croissant"
        elif (sens == "-"):
            print "d‚croissant"
        print "Interleaving :",
        if (interleaving == "p"):
            print "physical"
        elif (interleaving == "f"):
            print "fast load"
        else:
            print "Dos 3.3"
        print
	record = bufDSK.pack(*modifiedDSK)      # on repack la liste modif‚e vers la structure
	fDSK.seek(0)			        # on remet … zero le file pointer (pour tout r‚‚crire)
        fDSK.write(record)                   	# ecriture vers fichier sortie de la structure
        print "-> fichier",nameDSK,"modifi‚"

    # fin - nettoyage / fermeture fichiers
    fbin.close()                                # fermeture fichier binaire
    fDSK.close()                                # fermeture fichier DSK

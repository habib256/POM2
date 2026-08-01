# -*- coding:cp437 -*-
# WARNIG : encodage pour Console DOS sous Windows uniquement !!!

# InsertBIN 0.3 (8.2019)
# based on :
# InsertZic 0.21/Direct Write 0.42
# (c) French Touch
version = "0.3 (08.2019)"

import sys
import struct


def FillNFO(modDSK, nameBinary, track, sector):
    global base
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
    

    # sens incremental pour copier le bin dans le DSK
    t = track
    s = sector
    j = 0
    k = 0
    while j<nbSector:
        s1 = s                          # 
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
            
    s2 = s-1
    if s2<0:
        s2 = 0xF
        t2 = t-1
    else:
        t2 = t
    
    print 
    print 'Inject',nameBinary,':',hex(nbSector),'sectors ({:#X} bytes) from'.format(lenmodBin),
    print "T{:02X}/S{:02X} to T{:02X}/S{:02X} [Offset ${:04X}]".format(track,sector,t2,s2,base)

    base = base + 0x100*nbSector # modification variable globale

    # fin - nettoyage / fermeture fichier
    fbin.close()    # fermeture fichier binaire
    
    return t, s # on retourne la valeur courante du prochain couple piste/secteur o— l'on pourra ‚crire


if __name__ == '__main__':

    print
    print "InsertBIN version",version
    print

    if len(sys.argv) < 6:
        nameDSK = raw_input("Image Disk : ")
        trackd = int(raw_input("Firt Track (Hexa) : "),16)
        sectord = int(raw_input("First Sector (Hexa) : "),16)
        base = int(raw_input("Address base (Hexa) : "),16)
        nfoList = raw_input("List des nfo (xxx,yyy,zzz) : ")
    else:    
        nameDSK = sys.argv[1]
        trackd = int(sys.argv[2],16)
        sectord = int(sys.argv[3],16)
        base = int(sys.argv[4],16)
        nfoList = sys.argv[5]

    str1 = nfoList.split(',')                   # on r‚cupŠre l'argument dans une liste
    ListNFO = [x.strip() for x in str1]         # compr‚hension de list pour supprimer les espaces

    fDSK = open(nameDSK,"rb+")                  # ouverture fichier DSK (lecture + modification)
    record = fDSK.read()                        # lecture complŠte
    if len(record) != 143360:                   # v‚rification taille standard d'un fichier DSK
        print ("problem with DSK file")
    else :
        bufDSK = struct.Struct("<143360B")      # structure fichier DSK (143360 x 1 byte)
        outDSK = bufDSK.unpack(record)          # on unpack le fichier DSK vers la structure d‚finie
        modifiedDSK = bytearray(outDSK)         # on cr‚e une bytearray … partir du contenu pour pouvoir la modifier

    track = trackd
    sector = sectord
    
    for nfo in ListNFO:
        track, sector = FillNFO(modifiedDSK,nfo,track,sector)


    record = bufDSK.pack(*modifiedDSK)          # on repack la liste modif‚e vers la structure
    fDSK.seek(0)			        # on remet … zero le file pointer (pour tout r‚‚crire)
    fDSK.write(record)                   	# ecriture vers fichier sortie de la structure
    print
    print "-> file",nameDSK,"modified and saved!"

    fDSK.close()                                # fermeture fichier DSK

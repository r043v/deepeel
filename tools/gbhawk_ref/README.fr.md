# gbh : trace de référence GBHawk (BizHawk)

Rejoue un film BK2 (ou un journal d'entrées `|UDLRsSBA P|`) sur le cœur **GBHawk** de BizHawk, sans interface, et écrit
une ligne par trame : `trame PC SP AF BC DE HL hash-écran`. Sert à comparer m0boy au moteur qui a enregistré le film.
Aucun binaire ni ROM dans le dépôt.

Construction (dotnet SDK requis ; la bibliothèque BizHawk est construite hors dépôt) :

    dotnet build /chemin/BizHawk/src/BizHawk.Emulation.Cores -c Release -o $BHBIN
    dotnet build tools/gbhawk_ref -c Release -o out -p:BHBIN=$BHBIN
    cp $BHBIN/*.dll out/; cp /chemin/BizHawk/Assets/dll/lib{blip_buf,bizhash}.so out/

Utilisation : `dotnet out/gbh.dll ROM.gb entrees.txt trace.txt DMG_BOOT.bin [prefixe-capture N]`.

La boot ROM DMG d'origine est **obligatoire** (GBHawk refuse de démarrer sans) ; l'outil affiche son SHA-1 et dit si
c'est celle du film (`4ED31EC6B0B175BB109C0EB5FD3D193DA823339F`). Elle n'est pas fournie et n'est pas dans ce dépôt.

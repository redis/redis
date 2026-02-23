This tool is similar to redis-cli (but very basic) but allows
to specify arguments that are expanded as vectors by calling
ollama to get the embedding.

Whatever is passed as !"foo bar" gets expanded into
    VALUES ... embedding ...

You must have ollama running with the mxbai-emb-large model
already installed for this to work.

Example:

    redis> KEYS *
    1) food_items
    2) glove_embeddings_bin
    3) many_movies_mxbai-embed-large_BIN
    4) many_movies_mxbai-embed-large_NOQUANT
    5) word_embeddings
    6) word_embeddings_bin
    7) glove_embeddings_fp32

    redis> VSIM food_items !"drinks with fruit"
    1) (Fruit)Juices,Lemonade,100ml,50 cal,210 kJ
    2) (Fruit)Juices,Limeade,100ml,128 cal,538 kJ
    3) CannedFruit,Canned Fruit Cocktail,100g,81 cal,340 kJ
    4) (Fruit)Juices,Energy-Drink,100ml,87 cal,365 kJ
    5) Fruits,Lime,100g,30 cal,126 kJ
    6) (Fruit)Juices,Coconut Water,100ml,19 cal,80 kJ
    7) Fruits,Lemon,100g,29 cal,122 kJ
    8) (Fruit)Juices,Clamato,100ml,60 cal,252 kJ
    9) Fruits,Fruit salad,100g,50 cal,210 kJ
    10) (Fruit)Juices,Capri-Sun,100ml,41 cal,172 kJ

    redis> vsim food_items !"barilla"
    1) Pasta&Noodles,Spirelli,100g,367 cal,1541 kJ
    2) Pasta&Noodles,Farfalle,100g,358 cal,1504 kJ
    3) Pasta&Noodles,Capellini,100g,353 cal,1483 kJ
    4) Pasta&Noodles,Spaetzle,100g,368 cal,1546 kJ
    5) Pasta&Noodles,Cappelletti,100g,164 cal,689 kJ
    6) Pasta&Noodles,Penne,100g,351 cal,1474 kJ
    7) Pasta&Noodles,Shells,100g,353 cal,1483 kJ
    8) Pasta&Noodles,Linguine,100g,357 cal,1499 kJ
    9) Pasta&Noodles,Rotini,100g,353 cal,1483 kJ
    10) Pasta&Noodles,Rigatoni,100g,353 cal,1483 kJ

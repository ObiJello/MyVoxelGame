# B8 block-entity inventory (Java --dump-block-entities across all gate regions)
# Regenerable: run the 15-region java_be batch (see memory). One example per type.

## (no-top-id)  (count 180, example from city)
```
E,5,-51,7,{id:"DUMMY"}
```

## minecraft:brushable_block  (count 151, example from coldruin)
```
E,6,50,11,{LootTable:"minecraft:archaeology/ocean_ruin_cold",LootTableSeed:6092165544387225782l,components:{},id:"minecraft:brushable_block"}
```

## minecraft:chest  (count 69, example from beached)
```
E,4,59,8,{LootTable:"minecraft:chests/shipwreck_supply",LootTableSeed:-4096580202977426073l,components:{},id:"minecraft:chest"}
```

## minecraft:decorated_pot  (count 40, example from trialch)
```
E,11,-35,14,{LootTable:"minecraft:pots/trial_chambers/corridor",LootTableSeed:5740347877704302644l,components:{},id:"minecraft:decorated_pot"}
```

## minecraft:bed  (count 32, example from trialch)
```
E,2,-30,2,{components:{},id:"minecraft:bed"}
```

## minecraft:vault  (count 26, example from trialch)
```
E,12,-27,4,{components:{},config:{key_item:{count:1,id:"minecraft:trial_key"}},id:"minecraft:vault",server_data:{},shared_data:{}}
```

## minecraft:barrel  (count 13, example from trialch)
```
E,4,-29,11,{Items:[],components:{},id:"minecraft:barrel"}
```

## minecraft:mob_spawner  (count 12, example from city)
```
E,9,43,3,{Delay:20s,MaxNearbyEntities:6s,MaxSpawnDelay:800s,MinSpawnDelay:200s,RequiredPlayerRange:16s,SpawnCount:4s,SpawnData:{entity:{id:"minecraft:skeleton"}},SpawnPotentials:[],SpawnRange:4s,components:{},id:"minecraft:mob_spawner"}
```

## minecraft:trial_spawner  (count 12, example from trialch)
```
E,7,-28,2,{components:{},id:"minecraft:trial_spawner",normal_config:"minecraft:trial_chamber/ranged/poison_skeleton/normal",ominous_config:"minecraft:trial_chamber/ranged/poison_skeleton/ominous"}
```

## minecraft:banner  (count 9, example from mansion)
```
E,5,75,4,{components:{},id:"minecraft:banner"}
```

## minecraft:dispenser  (count 8, example from jungle)
```
E,3,71,1,{LootTable:"minecraft:chests/jungle_temple_dispenser",LootTableSeed:-2010785737271225186l,components:{},id:"minecraft:dispenser"}
```

## minecraft:hopper  (count 8, example from trialch)
```
E,4,-28,11,{Items:[],TransferCooldown:0,components:{},id:"minecraft:hopper"}
```

## minecraft:sculk_sensor  (count 4, example from city)
```
E,15,-45,12,{components:{},id:"minecraft:sculk_sensor",last_vibration_frequency:0,listener:{event_delay:0,selector:{tick:-1l}}}
```

## minecraft:furnace  (count 4, example from trailruins)
```
E,5,82,15,{Items:[],RecipesUsed:{},components:{},cooking_time_spent:0s,cooking_total_time:0s,id:"minecraft:furnace",lit_time_remaining:0s,lit_total_time:0s}
```

## minecraft:comparator  (count 2, example from city)
```
E,10,-49,10,{OutputSignal:0,components:{},id:"minecraft:comparator"}
```

## minecraft:blast_furnace  (count 2, example from trailruins)
```
E,5,81,15,{Items:[],RecipesUsed:{},components:{},cooking_time_spent:0s,cooking_total_time:0s,id:"minecraft:blast_furnace",lit_time_remaining:0s,lit_total_time:0s}
```

## minecraft:lectern  (count 1, example from city)
```
E,2,-49,10,{components:{},id:"minecraft:lectern"}
```

## minecraft:bell  (count 1, example from vplains)
```
E,2,65,12,{components:{},id:"minecraft:bell"}
```

## minecraft:sign  (count 1, example from igloolab)
```
E,3,60,1,{back_text:{color:"black",has_glowing_text:0b,messages:["","","",""]},components:{},front_text:{color:"black",has_glowing_text:0b,messages:["","<----","---->",""]},id:"minecraft:sign",is_waxed:0b}
```

## minecraft:brewing_stand  (count 1, example from igloolab)
```
E,5,60,4,{BrewTime:0s,Fuel:0b,Items:[{Slot:1b,components:{"minecraft:potion_contents":{potion:"minecraft:weakness"}},count:1,id:"minecraft:splash_potion"}],components:{},id:"minecraft:brewing_stand"}
```


## Notes
- `{id:"DUMMY"}` (count 180): Java's pending-block-entity placeholder — emitted
  by getBlockEntityNbtForSaving for BE-capable blocks placed during worldgen
  WITHOUT an explicit block entity (e.g. sculk shriekers/sensors placed by the
  sculk spreader, beds' second halves?, etc.). C++ must track "BE-capable block
  placed, no BE attached" positions and emit the same placeholder.
- Nested `id:` values in the type list above (trial_key, allium, spider, ...)
  are item/entity ids inside Items/sherds/spawn_data, not top-level BE types.
- The E payload is the BlockEntity's save format after a load/save round-trip:
  per-type DEFAULT fields always present (furnace timers, sign text struct,
  universal `components:{}`), with template-nbt values layered in, plus
  LootTableSeed from the already-parity nextLong draws.

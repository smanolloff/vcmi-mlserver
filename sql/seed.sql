--
-- Query to seed `stats` table with data.
-- Given the number of heroes, it will insert a row with wins=0 and games=0
--  for each possible pair of hero IDs within the given number of pools.
--
-- Parameters:
--  1: number of pools
--  2: number of heroes in one pool
--  3: the side from which stats are collected (0 for left, 1 for right side)
--
-- To seed for 2 pools, 4 heroes per pool and side 0:
--  $ cat seed.sql |
--      sed -e "s/--.*//" -e "1,/\?/s/\?/2/" -e "1,/\?/s/\?/4/" -e "1,/\?/s/\?/0/" |
--      sqlite3 stats.db
--
BEGIN;
WITH RECURSIVE
    config AS (
        SELECT ? AS n_pools, ? AS pool_size
    ),
    pools AS (
        SELECT 0 AS id
        UNION ALL
        SELECT id + 1
        FROM pools, config
        WHERE id + 1 < config.n_pools
    ),
    heroes AS (
        SELECT 0 AS id
        UNION ALL
        SELECT id + 1
        FROM heroes, config
        WHERE id + 1 < config.pool_size
    ),
    data AS (
        SELECT
            pools.id as pool,
            (pools.id * config.pool_size) + lheroes.id AS lhero,
            (pools.id * config.pool_size) + rheroes.id AS rhero
        FROM heroes AS lheroes, heroes AS rheroes, pools, config
        WHERE lheroes.id != rheroes.id
        ORDER BY pool, lhero, rhero
    )
INSERT INTO stats (pool, lhero, rhero, wins, games)
SELECT pool, lhero, rhero, 0, 0
FROM data;

INSERT INTO stats_md (side) VALUES (?);
COMMIT;

// =============================================================================
// Copyright 2024 Simeon Manolov <s.manolloff@gmail.com>.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "ServerPlugin.h"
#include "ArtifactUtils.h"
#include "BattleFieldHandler.h"
#include "Global.h"
#include "TerrainHandler.h"
#include "constants/EntityIdentifiers.h"
#include "gameState/CGameState.h"
#include "mapObjects/CGHeroInstance.h"
#include "mapObjects/CGTownInstance.h"
#include "mapping/CMap.h"
#include "mapping/CMapDefines.h"
#include "modding/IdentifierStorage.h"
#include "networkPacks/PacksForClient.h"
#include "networkPacks/PacksForClientBattle.h"
#include "networkPacks/StackLocation.h"
#include "server/CGameHandler.h"
#include "server/CVCMIServer.h"
#include "vstd/RNG.h"
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <regex>

VCMI_LIB_NAMESPACE_BEGIN

namespace ML {
    /*
     * Organize heroes into pools based on hero name
     * Format: "hero_<INT>_pool_<STR>"
     * Example: "hero_5123_pool_150k"
     */
    static std::map<std::string, HeroPool> InitHeroPools(std::vector<ConstTransitivePtr<CGHeroInstance>> allheroes) {
        auto pattern = std::regex(R"(^hero_\d+_pool_([0-9A-Za-z]+)$)");
        auto res = std::map<std::string, HeroPool> {};
        int poolid = 0;

        for (auto &hero : allheroes) {
            std::string poolname = "default";  // fallback poolname
            std::smatch matches;

            // Check if the entire string matches the pattern
            if (std::regex_match(hero->nameCustomTextId, matches, pattern))
                poolname = matches[1].str();

            if (res.count(poolname) == 0)
                res.insert({poolname, HeroPool(poolid++, poolname)});

            auto it = res.find(poolname);
            if (it == res.end()) {
                res.insert({poolname, HeroPool(poolid++, poolname)});
                it = res.find(poolname);
            }

            auto &pool = it->second;
            pool.heroes.push_back(hero);
        }

        std::cout << "Grouped heroes into " << res.size() << " pools\n";
        return res;
    }


    static std::map<const BattleFieldInfo*, std::vector<const TerrainType*>> InitBattleterrains(std::string battlefieldPattern) {
        auto res = std::map<const BattleFieldInfo*, std::vector<const TerrainType*>> {};
        auto lands = std::set<const TerrainType*> {};
        auto pattern = std::regex();

        if (battlefieldPattern.empty()) {
            pattern = std::regex(".");
        } else {
            pattern = std::regex(battlefieldPattern);
        }

        VLC->terrainTypeHandler->forEach([&res, &lands, &pattern] (const TerrainType * terrain, bool &_) {
            if (terrain->isLand() && terrain->isPassable()) {
                lands.insert(terrain);
            }

            for (const auto & bf : terrain->battleFields) {
                if (std::regex_search(bf.getInfo()->getJsonKey(), pattern)) {
                    res[bf.getInfo()].push_back(terrain);
                } else {
                    // std::cout << "Filtering out " << bf.getInfo()->getJsonKey() << "\n";
                }
            }
        });

        VLC->battlefieldsHandler->forEach([&res, &lands, &pattern](const BattleFieldInfo * bi, bool &_) {
            if (res.count(bi) == 0) {
                if (std::regex_search(bi->getJsonKey(), pattern)) {
                    if (bi->getJsonKey() == "core:ship_to_ship") {
                        // XXX: ship-to-ship battles are buggy
                        //      See https://github.com/vcmi/vcmi/issues/4781
                        // res[bi] = {VLC->terrainTypeHandler->getByIndex(TerrainId::WATER)}
                    } else {
                        res[bi].insert(res[bi].end(), lands.begin(), lands.end());
                    }
                } else {
                    // std::cout << "Filtering out " << bi->getJsonKey() << "\n";
                }
            }
        });

        if (!battlefieldPattern.empty()) {
            std::cout << "Filtered battlefields matching pattern: '" << battlefieldPattern << "'\n";

            if (res.size() == 0) {
                std::cout << "ALL BATTLEFIELDS WERE FILTERED OUT\n";
            }

            for (auto &[bi, terrains] : res) {
                std::cout << bi->getJsonKey() << " ->";
                for (auto &t : terrains) {
                    std::cout << " " << t->getJsonKey();
                }
                std::cout << "\n";
             }
         }

        return res;
    }


    static std::map<const CGHeroInstance*, std::array<CArtifactInstance*, 3>> InitWarMachines(CGameState * gs) {
        auto res = std::map<const CGHeroInstance*, std::array<CArtifactInstance*, 3>> {};

        for (const auto &h : gs->map->heroesOnMap) {
            res.insert({h, {
                ArtifactUtils::createArtifact(ArtifactID::BALLISTA),
                ArtifactUtils::createArtifact(ArtifactID::AMMO_CART),
                ArtifactUtils::createArtifact(ArtifactID::FIRST_AID_TENT)
            }});
        }
        return res;
    }

    static std::vector<CreatureID> InitCreatures() {
        auto res = std::vector<CreatureID>{};

        VLC->creatures()->forEach([&res](const Creature * cr, bool &stop) {
            // Invalid creatures (arrow towers, war machines, NOT_USED, etc. have lvl=0)
            if (cr->getLevel() > 0)
                res.push_back(cr->getId());
        });

        return res;
    }

    static std::unique_ptr<Stats> InitStats(CGameState * gs, Config config, int npools, int poolsize) {
        if (config.statsMode == "disabled")
            return nullptr;

        if (config.swapSides > 0) {
            THROW_FORMAT("Cannot track stats for %s when swapping sides", config.statsMode);
        }

        return std::make_unique<Stats>(
            config.statsStorage,
            config.statsMode == "blue",
            config.statsTimeout,
            config.statsPersistFreq,
            config.maxBattles,
            npools,
            poolsize
        );
    }

    ServerPlugin::ServerPlugin(CGameHandler * gh, CGameState * gs, Config & config)
    : gh(gh)
    , gs(gs)
    , config(config)
    , heropools(InitHeroPools(gs->map->heroesOnMap))
    , battleterrains(InitBattleterrains(config.battlefieldPattern))
    , alltowns(gs->map->towns)
    , allmachines(InitWarMachines(gs))
    , allcreatures(InitCreatures())
    , stats(InitStats(gs, config, heropools.size(), heropools.begin()->second.heroes.size()))
    , rng(std::mt19937(config.rngSeed ? config.rngSeed : gs->getRandomGenerator().nextInt(0, std::numeric_limits<int>::max())))
    {
        if (config.randomHeroes > 0) {
            for (auto &[poolname, pool] : heropools) {
                if (pool.heroes.size() % 2 != 0) {
                    throw std::runtime_error("An even number of heroes is required in each hero pool.");
                }
            }
        }

        /*
         * Give artifacts + leadership to all heroes
         * (ensures morale and luck will be 0)
         */
        auto artifacts = std::map<ArtifactPosition, ArtifactID> {
            {ArtifactPosition::NECK, ArtifactID(108)},  // pendantOfCourage
            {ArtifactPosition::MISC1, ArtifactID(50)},  // crestOfValor
            {ArtifactPosition::MISC2, ArtifactID(51)},  // glyphOfGallantry
            {ArtifactPosition::MISC3, ArtifactID(84)},  // spiritOfOppression
            {ArtifactPosition::MISC4, ArtifactID(85)},  // hourglassOfTheEvilHour
        };

        for (const auto &h : gs->map->heroesOnMap) {
            auto hh = const_cast<CGHeroInstance*>(h.get());

            for (auto &[pos, artid] : artifacts) {
                if (hh->artifactsWorn.find(pos) != hh->artifactsWorn.end())
                    hh->removeArtifact(pos);
                hh->putArtifact(pos, ArtifactUtils::createArtifact(artid));
            }

            hh->setSecSkillLevel(SecondarySkill::LEADERSHIP, 3, true);
        }
    }

    void ServerPlugin::setupBattleHook(const CGTownInstance *& town, TerrainId & terrain, BattleField & terType, ui32 & seed) {
        if (config.randomTerrainChance > 0 && battleterrains.size() > 0) {
            auto dist = std::uniform_int_distribution<>(0, 99);
            auto roll = dist(rng);
            if (roll < config.randomTerrainChance) {
                // XXX: client will render the "original" terrain texture,
                //      but that's only visual, as the newly set terrain
                //      is the one actually in effect.
                auto dist1 = std::uniform_int_distribution<>(0, battleterrains.size() - 1);
                auto it1 = battleterrains.begin();
                std::advance(it1, dist1(rng));
                auto &[bi, terrains] = *it1;

                auto dist2 = std::uniform_int_distribution<>(0, terrains.size() - 1);
                auto it2 = terrains.begin();
                std::advance(it2, dist2(rng));

                // modification by reference
                terType = bi->battlefield;
                terrain = (*it2)->getId();
            }
        }

        if (config.randomObstacles > 0 && (battlecounter % config.randomObstacles == 0)) {
            // modification by reference
            seed = gs->getRandomGenerator().nextInt(0, std::numeric_limits<int>::max());
        }

        if (config.townChance > 0) {
            auto dist = std::uniform_int_distribution<>(0, 99);
            auto roll = dist(rng);

            if (roll < config.townChance) {
                if (towncounter % alltowns.size() == 0) {
                    towncounter = 0;
                    std::shuffle(alltowns.begin(), alltowns.end(), rng);
                }

                town = alltowns.at(towncounter); // modification by reference
                ++towncounter;
            } else {
                town = nullptr; // modification by reference
            }
        }
    }

    void ServerPlugin::startBattleHook(
        const CArmedInstance *&army1,
        const CArmedInstance *&army2,
        const CGHeroInstance *&hero1,
        const CGHeroInstance *&hero2
    ) {
        bool swappingSides = (config.swapSides > 0 && (battlecounter % config.swapSides) == 0);
        // printf("battlecounter: %d, swapSides: %d, rem: %d\n", battlecounter, config.swapSides, battlecounter % config.swapSides);

        if (!(hero1 && hero2))
            throw std::runtime_error("Both hero1 and hero2 are required");

        battlecounter++;

        if (swappingSides)
            redside = !redside;

        // printf("config.randomHeroes = %d\n", config.randomHeroes);

        if (config.randomHeroes > 0) {
            if (poolcounter % heropools.size() == 0)
                poolcounter = 0; // no shuffling for std::map

            auto it = heropools.begin();
            std::advance(it, poolcounter);
            auto &pool = it->second;

            if (pool.counter % pool.heroes.size() == 0) {
                pool.counter = 0;
                std::shuffle(pool.heroes.begin(), pool.heroes.end(), rng);

                // for (int i=0; i<allheroes.size(); i++)
                //  printf("allheroes[%d] = %s\n", i, allheroes.at(i)->getNameTextID().c_str());
            }
            // printf("poolcounter = %d\n", poolcounter);

            // XXX: heroes must be different (objects must have different tempOwner)
            // modification by reference
            hero1 = pool.heroes.at(pool.counter);
            hero2 = pool.heroes.at(pool.counter+1);

            // printf("Pool: %s, hero0: %s, hero1: %s\n", pool.name.c_str(), hero1->nameCustomTextId.c_str(), hero2->nameCustomTextId.c_str());

            if (battlecounter % config.randomHeroes == 0) {
                poolcounter += 1;
                pool.counter += 2;
            }

            // std::cout << "Pool: " << it->first << ", " << hero1->nameCustomTextId << " vs. " << hero2->nameCustomTextId << "\n";
        }

        if (config.warmachineChance > 0) {
            // XXX: adding war machines by index of pre-created per-hero artifact instances
            // 0=ballista, 1=cart, 2=tent
            auto machineslots = std::map<ArtifactID, ArtifactPosition> {
                {ArtifactID::BALLISTA, ArtifactPosition::MACH1},
                {ArtifactID::AMMO_CART, ArtifactPosition::MACH2},
                {ArtifactID::FIRST_AID_TENT, ArtifactPosition::MACH3},
            };

            auto dist = std::uniform_int_distribution<>(0, 99);
            for (auto h : {hero1, hero2}) {
                for (auto m : allmachines.at(h)) {
                    auto it = machineslots.find(m->getTypeId());
                    if (it == machineslots.end())
                        throw std::runtime_error("Could not find warmachine");

                    auto apos = it->second;
                    auto roll = dist(rng);
                    if (roll < config.warmachineChance) {
                        if (!h->getArt(apos))
                            const_cast<CGHeroInstance*>(h)->putArtifact(apos, m);
                    } else {
                        if (h->getArt(apos))
                            const_cast<CGHeroInstance*>(h)->removeArtifact(apos);
                    }
                }
            }
        }

        if (config.tightFormationChance > 0) {
            for (auto h : {hero1, hero2}) {
                auto dist = std::uniform_int_distribution<>(0, 99);
                auto roll = dist(rng);
                const_cast<CGHeroInstance*>(h)->formation = (roll < config.tightFormationChance)
                    ? EArmyFormation::TIGHT
                    : EArmyFormation::LOOSE;
            }
        }

        // Randomize mana
        auto dist = std::uniform_int_distribution<>(config.manaMin, config.manaMax);
        for(auto h : {hero1, hero2}) {
            if(!h) continue;
            auto sm = SetMana();
            auto roll = dist(rng);
            sm.val = roll;
            sm.hid = h->id;
            gh->sendAndApply(sm);
        }

        // Set temp owner of both heroes to player0 and player1
        // XXX: causes UB after battle, unless it is replayed (ok for training)
        // XXX: if redside=1 (right), hero2 should have owner=0 (red)
        //      if redside=0 (left), hero1 should have owner=0 (red)
        const_cast<CGHeroInstance*>(hero1)->tempOwner = PlayerColor(redside);
        const_cast<CGHeroInstance*>(hero2)->tempOwner = PlayerColor(!redside);

        // modification by reference
        army1 = static_cast<const CArmedInstance*>(hero1);
        army2 = static_cast<const CArmedInstance*>(hero2);

        if (config.randomStackChance) {
            auto dist100 = std::uniform_int_distribution<>(0, 99);
            auto distCreatures = std::uniform_int_distribution<>(0, allcreatures.size() - 1);
            auto distQuantity = std::uniform_int_distribution<>(0, 1000);

            auto maybeAddStack = [this, &dist100, &distQuantity, &distCreatures](const CGHeroInstance *hero, int slot) {
                if (hero->hasStackAtSlot(SlotID(slot))) {
                    if (hero->stacksCount() == 1)
                        return;

                    // Roll whether to remove existing stack
                    if (dist100(rng) >= config.randomStackChance)
                        return;

                    gh->eraseStack(StackLocation(hero, SlotID(slot)));
                }

                // Roll whether to add a new stack
                if (dist100(rng) >= config.randomStackChance)
                    return;

                auto * cr = allcreatures.at(distCreatures(rng)).toCreature();
                auto qty = distQuantity(rng);
                // Crude guard against absurd stacks such as 1K azure dragons
                qty = std::max(1, qty / cr->getLevel());
                // printf("Adding %d %s to slot %d for hero %s\n", qty, cr->getNamePluralTranslated().c_str(), slot, hero->nameCustomTextId.c_str());
                gh->insertNewStack(StackLocation(hero, SlotID(slot)), cr, qty);
            };

            for (auto &h : {hero1, hero2}) {
                for (int i=0; i<7; ++i) {
                    maybeAddStack(h, i);
                }
            }
        }
    }

    void ServerPlugin::endBattleHook(
        BattleResult * br,
        const CGHeroInstance * heroAttacker,
        const CGHeroInstance * heroDefender
    ) {
        // don't record stats for retreats (i.e. env resets)
        if (stats && br->result == EBattleResult::NORMAL) {
            auto extractHeroID = [](std::string name) {
                std::regex pattern(R"(^hero_(\d+)_pool_([0-9A-Za-z]+)$)");

                int hero_id = -1;
                std::string pool_name = "default";
                std::smatch match;

                if (std::regex_match(name, match, pattern)) {
                    pool_name = match[2].str();
                } else {
                    // assert old hero name format (no pools)
                    std::regex pattern2(R"(^hero_(\d+)$)");
                    if (!std::regex_match(name, match, pattern2))
                        throw std::runtime_error("invalid hero name: " + name);
                }

                hero_id = std::stoi(match[1].str());

                return std::pair<int, std::string>(hero_id, pool_name);
            };

            auto [attackerID, poolname] = extractHeroID(heroAttacker->nameCustomTextId);
            auto [defenderID, poolname2] = extractHeroID(heroDefender->nameCustomTextId);

            if (poolname != poolname2)
                throw std::runtime_error("Pools do not match: " + poolname + " <> " + poolname2);

            auto it = heropools.find(poolname);
            if (it == heropools.end())
                throw std::runtime_error("Could not pool with name: " + poolname);

            auto poolid = it->second.id;
            auto statside = (config.statsMode == "red") ? redside : !redside;
            auto victory = br->winner == BattleSide(statside);
            stats->dataadd(victory, poolid, attackerID, defenderID);
        }

        if (config.maxBattles && battlecounter >= config.maxBattles) {
            std::cout << "Hit battle limit of " << config.maxBattles << ", will quit now...\n";
            if (stats && config.statsPersistFreq) stats->dbupdate();

            #ifdef VCMI_APPLE
                ::exit(EXIT_SUCCESS);
            #else
                std::quick_exit(EXIT_SUCCESS);
            #endif

            ::exit(EXIT_SUCCESS);
            return;
        }
    }

}

VCMI_LIB_NAMESPACE_END

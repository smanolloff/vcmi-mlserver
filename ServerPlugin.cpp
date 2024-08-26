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
#include "Global.h"
#include "constants/EntityIdentifiers.h"
#include "gameState/CGameState.h"
#include "mapObjects/CGHeroInstance.h"
#include "mapObjects/CGTownInstance.h"
#include "mapping/CMap.h"
#include "networkPacks/PacksForClient.h"
#include "networkPacks/PacksForClientBattle.h"
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
    static std::map<std::string, std::pair<HeroPool, int>> InitHeroPools(HeroPool allheroes) {
        auto pattern = std::regex(R"(^hero_\d+_pool_([0-9A-Za-z]+)$)");
        auto res = std::map<std::string, std::pair<HeroPool, int>> {};

        for (auto &hero : allheroes) {
            std::string pool = "default";  // fallback pool
            std::smatch matches;

            // Check if the entire string matches the pattern
            if (std::regex_match(hero->nameCustomTextId, matches, pattern))
                pool = matches[1].str();

            auto &[poolheroes, _] = res[pool];
            poolheroes.push_back(hero);
        }

        std::cout << "Grouped heroes into " << res.size() << " pools\n";
        return res;
    }

    static std::map<const CGHeroInstance*, std::array<CArtifactInstance*, 3>> InitWarMachines(CGameState * gs) {
        auto res = std::map<const CGHeroInstance*, std::array<CArtifactInstance*, 3>> {};

        for (const auto &h : gs->map->heroesOnMap) {
            res.insert({h, {
                ArtifactUtils::createNewArtifactInstance(ArtifactID::BALLISTA),
                ArtifactUtils::createNewArtifactInstance(ArtifactID::AMMO_CART),
                ArtifactUtils::createNewArtifactInstance(ArtifactID::FIRST_AID_TENT)
            }});
        }
        return res;
    }

    // static
    std::unique_ptr<Stats> InitStats(CGameState * gs, Config config) {
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
            config.maxBattles
        );
    }

    ServerPlugin::ServerPlugin(CGameHandler * gh, CGameState * gs, Config & config)
    : gh(gh)
    , gs(gs)
    , config(config)
    , heropools(InitHeroPools(gs->map->heroesOnMap))
    , alltowns(gs->map->towns)
    , allmachines(InitWarMachines(gs))
    , stats(InitStats(gs, config))
    , rng(std::mt19937(gs->getRandomGenerator().nextInt(0, std::numeric_limits<int>::max())))
    {
        if (config.randomHeroes > 0) {
            for (auto &[pool, pooldata] : heropools) {
                auto &[poolheroes, _] = pooldata;
                if (poolheroes.size() % 2 != 0) {
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
                hh->putArtifact(pos, ArtifactUtils::createNewArtifactInstance(artid));
            }

            hh->setSecSkillLevel(SecondarySkill::LEADERSHIP, 3, true);
        }
    }

    void ServerPlugin::setupBattleHook(const CGTownInstance *& town, ui32 & seed) {
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

        battlecounter++;

        if (swappingSides)
            redside = !redside;

        // printf("config.randomHeroes = %d\n", config.randomHeroes);

        if (config.randomHeroes > 0) {
            // pick a random pool of heroes
            auto it = heropools.begin();
            std::advance(it, std::rand() % heropools.size());
            // auto &pool = it->first;
            auto &[poolheroes, poolcounter] = it->second;

            if (poolcounter % poolheroes.size() == 0) {
                poolcounter = 0;

                std::shuffle(poolheroes.begin(), poolheroes.end(), rng);

                // for (int i=0; i<allheroes.size(); i++)
                //  printf("allheroes[%d] = %s\n", i, allheroes.at(i)->getNameTextID().c_str());
            }
            // printf("poolcounter = %d\n", poolcounter);

            // XXX: heroes must be different (objects must have different tempOwner)
            // modification by reference
            hero1 = poolheroes.at(poolcounter);
            hero2 = poolheroes.at(poolcounter+1);

            if (battlecounter % config.randomHeroes == 0)
                poolcounter += 2;

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

        // Randomize mana
        auto dist = std::uniform_int_distribution<>(config.manaMin, config.manaMax);
        for(auto h : {hero1, hero2}) {
            if(!h) continue;
            auto sm = SetMana();
            auto roll = dist(rng);
            sm.val = roll;
            sm.hid = h->id;
            gh->sendAndApply(&sm);
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
    }

    void ServerPlugin::endBattleHook(
        BattleResult * br,
        const CGHeroInstance * heroAttacker,
        const CGHeroInstance * heroDefender
    ) {
        // don't record stats for retreats (i.e. env resets)
        if (stats && br->result == EBattleResult::NORMAL) {
            auto extractHeroID = [](std::string name) {
                auto pos = name.find('_');
                if (pos == std::string::npos)
                    throw std::runtime_error("No underscore found in hero name: " + name);
                auto num_str = name.substr(pos + 1);
                auto num = -1;

                try {
                    num = std::stoi(num_str);
                } catch (const std::exception &e) {
                    throw std::runtime_error("Could not extract hero ID from name: " + name + ": " + e.what());
                }

                return num;
            };

            auto attackerID = extractHeroID(heroAttacker->nameCustomTextId);
            auto defenderID = extractHeroID(heroDefender->nameCustomTextId);
            auto statside = (config.statsMode == "red") ? redside : !redside;
            auto victory = br->winner == BattleSide(statside);
            stats->dataadd(victory, attackerID, defenderID);
        }

        if (config.maxBattles && battlecounter >= config.maxBattles) {
            std::cout << "Hit battle limit of " << config.maxBattles << ", will quit now...\n";
            if (stats && config.statsPersistFreq) stats->dbupdate();
            exit(0); // XXX: is there any way to exit gracefully?
            return;
        }
    }

}

VCMI_LIB_NAMESPACE_END

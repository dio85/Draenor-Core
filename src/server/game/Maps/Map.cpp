////////////////////////////////////////////////////////////////////////////////
//
// Project-Hellscream https://hellscream.org
// Copyright (C) 2018-2020 Project-Hellscream-6.2
// Discord https://discord.gg/CWCF3C9
//
////////////////////////////////////////////////////////////////////////////////

#include "Map.h"
#include "GridStates.h"
#include "ScriptMgr.h"
#include "VMapFactory.h"
#include "MMapFactory.h"
#include "MapInstanced.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Transport.h"
#include "InstanceScript.h"
#include "ObjectAccessor.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "Group.h"
#include "LFGMgr.h"
#include "DynamicTree.h"
#include "Vehicle.h"
#include "WildBattlePet.h"
#include "OutdoorPvPMgr.h"
#include "DisableMgr.h"
#include "Logger.h"
#include "GridMap.h"
#include "MapDefines.h"

#define DEFAULT_GRID_EXPIRY     300
#define MAX_GRID_LOAD_TIME      50
#define MAX_CREATURE_ATTACK_RADIUS  (45.0f * sWorld->getRate(RATE_CREATURE_AGGRO))

GridState* si_GridStates[MAX_GRID_STATE];

template void Map::AddToActive<GameObject>(GameObject* obj);
template void Map::AddToActive<Corpse>(Corpse* obj);

Map::~Map()
{
    sScriptMgr->OnDestroyMap(this);

    // We need to depopulate WildBattlePet for respawn replaced creatures next time
    // Because respawn time is saved in database
    sWildBattlePetMgr->DepopulateMap(GetId());

    UnloadAll();

    while (!i_worldObjects.empty())
    {
        WorldObject* obj = *i_worldObjects.begin();
        ASSERT(obj->IsWorldObject());
        //ASSERT(obj->GetTypeId() == TYPEID_CORPSE);
        obj->SetZoneScript();
        obj->RemoveFromWorld();
        obj->ResetMap();
    }

    if (!m_scriptSchedule.empty())
        sScriptMgr->DecreaseScheduledScriptCount(m_scriptSchedule.size());

    MMAP::MMapFactory::createOrGetMMapManager()->unloadMapInstance(GetId(), i_InstanceId);
}

NGridType* Map::getNGrid(uint32 x, uint32 y) const
{
    //ASSERT(x < MAX_NUMBER_OF_GRIDS && y < MAX_NUMBER_OF_GRIDS);
    if (x > MAX_NUMBER_OF_GRIDS && y > MAX_NUMBER_OF_GRIDS)
    {
        TC_LOG_ERROR("server.worldserver", "CRASH::map::setNGrid() Invalid grid coordinates found: %d, %d!", x, y);
        return NULL;
    }
    return i_grids[x][y];
}

bool Map::ExistMap(uint32 mapid, int gx, int gy)
{
    int len = sWorld->GetDataPath().length()+strlen("maps/%04u_%02u_%02u.map")+1;
    char* tmp = new char[len];
    snprintf(tmp, len, (char *)(sWorld->GetDataPath()+"maps/%04u_%02u_%02u.map").c_str(), mapid, gx, gy);

    bool ret = false;
    FILE* pf=fopen(tmp, "rb");

    if (!pf)
        TC_LOG_ERROR("maps", "Map file '%s': does not exist!", tmp);
    else
    {
        map_fileheader header;
        if (fread(&header, sizeof(header), 1, pf) == 1)
        {
            if (header.mapMagic != MapMagic.asUInt || header.versionMagic != MapVersionMagic.asUInt)
                TC_LOG_ERROR("maps", "Map file '%s' is from an incompatible clientversion. Please recreate using the mapextractor.", tmp);
            else
                ret = true;
        }
        fclose(pf);
    }
    delete [] tmp;
    return ret;
}

bool Map::ExistVMap(uint32 mapid, int gx, int gy)
{
    if (VMAP::IVMapManager* vmgr = VMAP::VMapFactory::createOrGetVMapManager())
    {
        if (vmgr->isMapLoadingEnabled())
        {
            bool exists = vmgr->existsMap((sWorld->GetDataPath()+ "vmaps").c_str(),  mapid, gx, gy);
            if (!exists)
            {
                std::string name = vmgr->getDirFileName(mapid, gx, gy);
                TC_LOG_ERROR("maps", "VMap file '%s' is missing or points to wrong version of vmap file. Redo vmaps with latest version of vmap_assembler.exe.", (sWorld->GetDataPath()+"vmaps/"+name).c_str());
                return false;
            }
        }
    }

    return true;
}

void Map::LoadMMap(int gx, int gy)
{
    if (!DisableMgr::IsPathfindingEnabled(GetId()))
        return;

    bool mmapLoadResult = MMAP::MMapFactory::createOrGetMMapManager()->loadMap((sWorld->GetDataPath() + "mmaps").c_str(), GetId(), gx, gy);

    if (mmapLoadResult)
        TC_LOG_DEBUG("maps", "MMAP loaded name:%s, id:%d, x:%d, y:%d (mmap rep.: x:%d, y:%d)", GetMapName(), GetId(), gx, gy, gx, gy);
    else
        TC_LOG_DEBUG("maps", "Could not load MMAP name:%s, id:%d, x:%d, y:%d (mmap rep.: x:%d, y:%d)", GetMapName(), GetId(), gx, gy, gx, gy);
}

void Map::LoadVMap(int gx, int gy)
{
                                                            // x and y are swapped !!
    int vmapLoadResult = VMAP::VMapFactory::createOrGetVMapManager()->loadMap((sWorld->GetDataPath()+ "vmaps").c_str(),  GetId(), gx, gy);
    switch (vmapLoadResult)
    {
        case VMAP::VMAP_LOAD_RESULT_OK:
            TC_LOG_INFO("maps", "VMAP loaded name:%s, id:%d, x:%d, y:%d (vmap rep.: x:%d, y:%d)", GetMapName(), GetId(), gx, gy, gx, gy);
            break;
        case VMAP::VMAP_LOAD_RESULT_ERROR:
            TC_LOG_INFO("maps", "Could not load VMAP name:%s, id:%d, x:%d, y:%d (vmap rep.: x:%d, y:%d)", GetMapName(), GetId(), gx, gy, gx, gy);
            break;
        case VMAP::VMAP_LOAD_RESULT_IGNORED:
            TC_LOG_DEBUG("maps", "Ignored VMAP name:%s, id:%d, x:%d, y:%d (vmap rep.: x:%d, y:%d)", GetMapName(), GetId(), gx, gy, gx, gy);
            break;
    }
}
void Map::LoadMap(int gx, int gy, bool reload)
{
    LoadMapImpl(this, gx, gy, reload);
}

void Map::LoadMapImpl(Map* map, int gx, int gy, bool reload)
{
    if (map->i_InstanceId != 0)
    {
        if (map->GridMaps[gx][gy])
            return;
	
        // load grid map for base map
        if (!map->m_parentMap->GridMaps[gx][gy])
            map->m_parentMap->EnsureGridCreated(GridCoord(63-gx, 63-gy));

        ((MapInstanced*)(map->m_parentMap))->AddGridMapReference(GridCoord(gx, gy));
        map->GridMaps[gx][gy] = map->m_parentMap->GridMaps[gx][gy];
        return;
    }

    if (map->GridMaps[gx][gy] && !reload)
        return;

    //map already load, delete it before reloading (Is it necessary? Do we really need the ability the reload maps during runtime?)
    if (map->GridMaps[gx][gy])
    {
        TC_LOG_INFO("maps", "Unloading previously loaded map %u before reloading.", map->GetId());

        delete (map->GridMaps[gx][gy]);
        map->GridMaps[gx][gy]=NULL;
    }

    // map file name
    char* tmp = NULL;
    int len = sWorld->GetDataPath().length() + strlen("maps/%04u_%02u_%02u.map") + 1;
    tmp = new char[len];
    snprintf(tmp, len, (char*)(sWorld->GetDataPath() + "maps/%04u_%02u_%02u.map").c_str(), map->GetId(), gx, gy);
    // loading data
    map->GridMaps[gx][gy] = new GridMap();
    GridMap::LoadResult gridMapLoadResult = map->GridMaps[gx][gy]->loadData(tmp);

    if (gridMapLoadResult == GridMap::LoadResult::InvalidFile)
        TC_LOG_ERROR("maps", "Error loading map file: %s", tmp);
   
    sScriptMgr->OnLoadGridMap(map, map->GridMaps[gx][gy], gx, gy);
}

void Map::LoadMapAndVMap(int gx, int gy)
{
    LoadMap(gx, gy);
    if (i_InstanceId == 0)
    {
        LoadVMap(gx, gy);                                   // Only load the data for the base map
        LoadMMap(gx, gy);
    }
}

void Map::InitStateMachine()
{
    si_GridStates[GRID_STATE_INVALID] = new InvalidState;
    si_GridStates[GRID_STATE_ACTIVE] = new ActiveState;
    si_GridStates[GRID_STATE_IDLE] = new IdleState;
    si_GridStates[GRID_STATE_REMOVAL] = new RemovalState;
}

void Map::DeleteStateMachine()
{
    delete si_GridStates[GRID_STATE_INVALID];
    delete si_GridStates[GRID_STATE_ACTIVE];
    delete si_GridStates[GRID_STATE_IDLE];
    delete si_GridStates[GRID_STATE_REMOVAL];
}

Map::Map(uint32 id, time_t expiry, uint32 InstanceId, uint8 SpawnMode, Map* _parent):
_creatureToMoveLock(false), _gameObjectsToMoveLock(false), i_mapEntry(sMapStore.LookupEntry(id)),
i_spawnMode(SpawnMode), i_InstanceId(InstanceId), m_unloadTimer(0), m_VisibleDistance(DEFAULT_VISIBILITY_DISTANCE),
m_VisibilityNotifyPeriod(DEFAULT_VISIBILITY_NOTIFY_PERIOD),
m_activeNonPlayersIter(m_activeNonPlayers.end()), _transportsGameObjectUpdateIter(_transportsGameObject.end()), _transportsUpdateIter(_transports.end()),
i_gridExpiry(expiry), i_scriptLock(false)
{
    m_parentMap = (_parent ? _parent : this);
    for (unsigned int idx=0; idx < MAX_NUMBER_OF_GRIDS; ++idx)
    {
        for (unsigned int j=0; j < MAX_NUMBER_OF_GRIDS; ++j)
        {
            //z code
            GridMaps[idx][j] =NULL;
            setNGrid(NULL, idx, j);
        }
    }

    //lets initialize visibility distance for map
    Map::InitVisibilityDistance();

    sScriptMgr->OnCreateMap(this);
}

void Map::InitVisibilityDistance()
{
    //init visibility for continents
    m_VisibleDistance = World::GetMaxVisibleDistanceOnContinents();
    m_VisibilityNotifyPeriod = World::GetVisibilityNotifyPeriodOnContinents();
}

void Map::SetObjectVisibility(float p_Visibility)
{
    m_VisibleDistance = p_Visibility;
}

// Template specialization of utility methods
template<class T>
void Map::AddToGrid(T* obj, Cell const& cell)
{
    NGridType* grid = getNGrid(cell.GridX(), cell.GridY());
    if (obj->IsWorldObject())
        grid->GetGridType(cell.CellX(), cell.CellY()).template AddWorldObject<T>(obj);
    else
        grid->GetGridType(cell.CellX(), cell.CellY()).template AddGridObject<T>(obj);
}

template<>
void Map::AddToGrid(Creature* obj, Cell const& cell)
{
    NGridType* grid = getNGrid(cell.GridX(), cell.GridY());
    if (obj->IsWorldObject())
        grid->GetGridType(cell.CellX(), cell.CellY()).AddWorldObject(obj);
    else
        grid->GetGridType(cell.CellX(), cell.CellY()).AddGridObject(obj);

    obj->SetCurrentCell(cell);
}

template<>
void Map::AddToGrid(GameObject* obj, Cell const& cell)
{
    NGridType* grid = getNGrid(cell.GridX(), cell.GridY());
    grid->GetGridType(cell.CellX(), cell.CellY()).AddGridObject(obj);

    obj->SetCurrentCell(cell);
}

template<class T>
void Map::SwitchGridContainers(T* /*obj*/, bool /*on*/)
{
}

template<>

void Map::SwitchGridContainers(Creature* obj, bool on)
{
    ASSERT(!obj->IsPermanentWorldObject());
    CellCoord p = Trinity::ComputeCellCoord(obj->GetPositionX(), obj->GetPositionY());
    if (!p.IsCoordValid())
    {
        TC_LOG_ERROR("maps", "Map::SwitchGridContainers: Object " UI64FMTD " has invalid coordinates X:%f Y:%f grid cell [%u:%u]", obj->GetGUID(), obj->GetPositionX(), obj->GetPositionY(), p.x_coord, p.y_coord);
        return;
    }

    Cell cell(p);
    if (!IsGridLoaded(GridCoord(cell.data.Part.grid_x, cell.data.Part.grid_y)))
        return;

    TC_LOG_DEBUG("maps", "Switch object " UI64FMTD " from grid[%u, %u] %u", obj->GetGUID(), cell.data.Part.grid_x, cell.data.Part.grid_y, on);
    NGridType *ngrid = getNGrid(cell.GridX(), cell.GridY());
    ASSERT(ngrid != NULL);

    GridType &grid = ngrid->GetGridType(cell.CellX(), cell.CellY());

    obj->RemoveFromGrid(); //This step is not really necessary but we want to do ASSERT in remove/add
    if (on)
    {
        grid.AddWorldObject(obj);
        AddWorldObject(obj);
    }
    else
    {
        grid.AddGridObject(obj);
        RemoveWorldObject(obj);
    }
    obj->m_isTempWorldObject = on;
}

template<>
void Map::SwitchGridContainers(GameObject* obj, bool on)
{
    ASSERT(!obj->IsPermanentWorldObject());
    CellCoord p = Trinity::ComputeCellCoord(obj->GetPositionX(), obj->GetPositionY());
    if (!p.IsCoordValid())
    {
        TC_LOG_ERROR("maps", "Map::SwitchGridContainers: Object " UI64FMTD " has invalid coordinates X:%f Y:%f grid cell [%u:%u]", obj->GetGUID(), obj->GetPositionX(), obj->GetPositionY(), p.x_coord, p.y_coord);
        return;
    }

    Cell cell(p);
    if (!IsGridLoaded(GridCoord(cell.data.Part.grid_x, cell.data.Part.grid_y)))
        return;

    TC_LOG_DEBUG("maps", "Switch object " UI64FMTD " from grid[%u, %u] %u", obj->GetGUID(), cell.data.Part.grid_x, cell.data.Part.grid_y, on);
    NGridType *ngrid = getNGrid(cell.GridX(), cell.GridY());
    ASSERT(ngrid != NULL);

    GridType &grid = ngrid->GetGridType(cell.CellX(), cell.CellY());

    obj->RemoveFromGrid(); //This step is not really necessary but we want to do ASSERT in remove/add

    if (on)
    {
        grid.AddWorldObject(obj);
        AddWorldObject(obj);
    }
    else
    {
        grid.AddGridObject(obj);
        RemoveWorldObject(obj);
    }
}


template<class T>
void Map::DeleteFromWorld(T* obj)
{
    // Note: In case resurrectable corpse and pet its removed from global lists in own destructor
    delete obj;
}

template<>
void Map::DeleteFromWorld(Player* player)
{
    sObjectAccessor->RemoveObject(player);
    sObjectAccessor->RemoveUpdateObject(player); //TODO: I do not know why we need this, it should be removed in ~Object anyway
    delete player;
}

//Create NGrid so the object can be added to it
//But object data is not loaded here
void Map::EnsureGridCreated(const GridCoord &p)
{
    if (!getNGrid(p.x_coord, p.y_coord))
    {
        TRINITY_GUARD(ACE_Thread_Mutex, Lock);
        if (!getNGrid(p.x_coord, p.y_coord))
        {
            TC_LOG_DEBUG("maps", "Creating grid[%u, %u] for map %u instance %u", p.x_coord, p.y_coord, GetId(), i_InstanceId);

            setNGrid(new NGridType(p.x_coord*MAX_NUMBER_OF_GRIDS + p.y_coord, p.x_coord, p.y_coord, i_gridExpiry, sWorld->getBoolConfig(CONFIG_GRID_UNLOAD)),
                p.x_coord, p.y_coord);

            // build a linkage between this map and NGridType
            buildNGridLinkage(getNGrid(p.x_coord, p.y_coord));

            getNGrid(p.x_coord, p.y_coord)->SetGridState(GRID_STATE_IDLE);

            //z coord
            int gx = (MAX_NUMBER_OF_GRIDS - 1) - p.x_coord;
            int gy = (MAX_NUMBER_OF_GRIDS - 1) - p.y_coord;

            if (!GridMaps[gx][gy])
                LoadMapAndVMap(gx, gy);
        }
    }
}

//Load NGrid and make it active
void Map::EnsureGridLoadedForActiveObject(const Cell &cell, WorldObject* object)
{
    EnsureGridLoaded(cell);
    NGridType *grid = getNGrid(cell.GridX(), cell.GridY());
    ASSERT(grid != NULL);

    // refresh grid state & timer
    if (grid->GetGridState() != GRID_STATE_ACTIVE)
    {
        TC_LOG_DEBUG("maps", "Active object " UI64FMTD " triggers loading of grid [%u, %u] on map %u", object->GetGUID(), cell.GridX(), cell.GridY(), GetId());
        ResetGridExpiry(*grid, 0.1f);
        grid->SetGridState(GRID_STATE_ACTIVE);
    }
}

//Create NGrid and load the object data in it
bool Map::EnsureGridLoaded(const Cell &cell)
{
    EnsureGridCreated(GridCoord(cell.GridX(), cell.GridY()));
    NGridType *grid = getNGrid(cell.GridX(), cell.GridY());

    ASSERT(grid != NULL);
    if (!isGridObjectDataLoaded(cell.GridX(), cell.GridY()))
    {
        TC_LOG_DEBUG("maps", "Loading grid[%u, %u] for map %u instance %u", cell.GridX(), cell.GridY(), GetId(), i_InstanceId);

        setGridObjectDataLoaded(true, cell.GridX(), cell.GridY());

        ObjectGridLoader loader(*grid, this, cell);
        loader.LoadN();

        // Add resurrectable corpses to world object list in grid
        sObjectAccessor->AddCorpsesToGrid(GridCoord(cell.GridX(), cell.GridY()), grid->GetGridType(cell.CellX(), cell.CellY()), this);
        Balance();
        return true;
    }

    return false;
}

void Map::LoadGrid(float x, float y)
{
    EnsureGridLoaded(Cell(x, y));
}

bool Map::AddPlayerToMap(Player* player, bool p_Switched /*= false*/)
{
    CellCoord cellCoord = Trinity::ComputeCellCoord(player->GetPositionX(), player->GetPositionY());
    if (!cellCoord.IsCoordValid())
    {
        TC_LOG_ERROR("maps", "Map::Add: Player (GUID: %u) has invalid coordinates X:%f Y:%f grid cell [%u:%u]", player->GetGUIDLow(), player->GetPositionX(), player->GetPositionY(), cellCoord.x_coord, cellCoord.y_coord);
        return false;
    }

    Cell cell(cellCoord);
    EnsureGridLoadedForActiveObject(cell, player);
    AddToGrid(player, cell);

    // Check if we are adding to correct map
    ASSERT (player->GetMap() == this);
    player->SetMap(this);
    player->AddToWorld();

    SendInitSelf(player, p_Switched);
    SendInitTransports(player);

    player->m_clientGUIDs.clear();
    player->UpdateObjectVisibility(false);

    sScriptMgr->OnPlayerEnterMap(this, player);
    sOutdoorPvPMgr->HandlePlayerEnterMap(player, GetId());
    return true;
}

template<class T>
void Map::InitializeObject(T* /*obj*/)
{
}

template<>
void Map::InitializeObject(Creature* obj)
{
    obj->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
}

template<>
void Map::InitializeObject(GameObject* obj)
{
    obj->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
}

template<class T>
bool Map::AddToMap(T* obj)
{
    //TODO: Needs clean up. An object should not be added to map twice.
    if (obj->IsInWorld())
    {
        ASSERT(obj->IsInGrid());
        obj->UpdateObjectVisibility(true);
        return true;
    }

    CellCoord cellCoord = Trinity::ComputeCellCoord(obj->GetPositionX(), obj->GetPositionY());
    //It will create many problems (including crashes) if an object is not added to grid after creation
    //The correct way to fix it is to make AddToMap return false and delete the object if it is not added to grid
    //But now AddToMap is used in too many places, I will just see how many ASSERT failures it will cause
    ASSERT(cellCoord.IsCoordValid());
    if (!cellCoord.IsCoordValid())
    {
        TC_LOG_ERROR("maps", "Map::Add: Object " UI64FMTD " has invalid coordinates X:%f Y:%f grid cell [%u:%u]", obj->GetGUID(), obj->GetPositionX(), obj->GetPositionY(), cellCoord.x_coord, cellCoord.y_coord);
        return false; //Should delete object
    }

    Cell cell(cellCoord);
    if (obj->isActiveObject())
        EnsureGridLoadedForActiveObject(cell, obj);
    else
        EnsureGridCreated(GridCoord(cell.GridX(), cell.GridY()));
    AddToGrid(obj, cell);
    TC_LOG_DEBUG("maps", "Object %u enters grid[%u, %u]", GUID_LOPART(obj->GetGUID()), cell.GridX(), cell.GridY());

    //Must already be set before AddToMap. Usually during obj->Create.
    //obj->SetMap(this);
    obj->AddToWorld();

    InitializeObject(obj);

    if (obj->isActiveObject())
        AddToActive(obj);

    //something, such as vehicle, needs to be update immediately
    //also, trigger needs to cast spell, if not update, cannot see visual
    obj->UpdateObjectVisibility(true);

    if (obj->ToCreature())
        sWildBattlePetMgr->OnAddToMap(obj->ToCreature());

    return true;
}

template<>
bool Map::AddToMap(Transport* obj)
{
    //TODO: Needs clean up. An object should not be added to map twice.
    if (obj->IsInWorld())
        return true;

    CellCoord cellCoord = Trinity::ComputeCellCoord(obj->GetPositionX(), obj->GetPositionY());
    if (!cellCoord.IsCoordValid())
    {
        TC_LOG_ERROR("maps", "Map::Add: Object " UI64FMTD " has invalid coordinates X:%f Y:%f grid cell [%u:%u]", obj->GetGUID(), obj->GetPositionX(), obj->GetPositionY(), cellCoord.x_coord, cellCoord.y_coord);
        return false; //Should delete object
    }

    obj->AddToWorld();
    _transports.insert(obj);

    return true;
}

bool Map::IsGridLoaded(const GridCoord &p) const
{
    return (getNGrid(p.x_coord, p.y_coord) && isGridObjectDataLoaded(p.x_coord, p.y_coord));
}

void Map::VisitNearbyCellsOf(WorldObject* obj, TypeContainerVisitor<Trinity::ObjectUpdater, GridTypeMapContainer> &gridVisitor, TypeContainerVisitor<Trinity::ObjectUpdater, WorldTypeMapContainer> &worldVisitor)
{
    // Check for valid position
    if (!obj->IsPositionValid())
        return;

    // Update mobs/objects in ALL visible cells around object!
    CellArea area = Cell::CalculateCellArea(obj->GetPositionX(), obj->GetPositionY(), obj->GetGridActivationRange());

    for (uint32 x = area.low_bound.x_coord; x <= area.high_bound.x_coord; ++x)
    {
        for (uint32 y = area.low_bound.y_coord; y <= area.high_bound.y_coord; ++y)
        {
            // marked cells are those that have been visited
            // don't visit the same cell twice
            uint32 cell_id = (y * TOTAL_NUMBER_OF_CELLS_PER_MAP) + x;
            if (isCellMarked(cell_id))
                continue;

            markCell(cell_id);
            CellCoord pair(x, y);
            Cell cell(pair);
            cell.SetNoCreate();
            Visit(cell, gridVisitor);
            Visit(cell, worldVisitor);
        }
    }
}

void Map::Update(const uint32 t_diff)
{
#ifdef CROSS
    SetUpdating(true);
#endif

    uint32 l_Time = getMSTime();

    _dynamicTree.update(t_diff);
    /// update worldsessions for existing players
    for (m_mapRefIter = m_mapRefManager.begin(); m_mapRefIter != m_mapRefManager.end(); ++m_mapRefIter)
    {
        Player* player = m_mapRefIter->getSource();
        if (player && player->IsInWorld())
        {
#ifdef CROSS
            if (player->IsNeedRemove())
                continue;

#endif

            WorldSession* session = player->GetSession();

#ifdef CROSS
            if (!session->GetPlayer())
                continue;
#endif

            MapSessionFilter updater(session);
            session->Update(t_diff, updater);
        }
    }
    /// update active cells around players and active objects
    resetMarkedCells();

    Trinity::ObjectUpdater updater(t_diff);
    // for creature
    TypeContainerVisitor<Trinity::ObjectUpdater, GridTypeMapContainer  > grid_object_update(updater);
    // for pets
    TypeContainerVisitor<Trinity::ObjectUpdater, WorldTypeMapContainer > world_object_update(updater);

    // the player iterator is stored in the map object
    // to make sure calls to Map::Remove don't invalidate it
    for (m_mapRefIter = m_mapRefManager.begin(); m_mapRefIter != m_mapRefManager.end(); ++m_mapRefIter)
    {
        Player* player = m_mapRefIter->getSource();

#ifndef CROSS
        if (!player || !player->IsInWorld())
#else /* CROSS */
        ASSERT(player->GetMap() == this);

        if (!player || !player->IsInWorld() || player->GetSession()->IsIRClosing())
            continue;

        if (player->IsNeedRemove())
            continue;

        WorldSession* session = player->GetSession();
        if (!session->GetPlayer())
#endif /* CROSS */
            continue;

        // update players at tick
        player->Update(t_diff);

        VisitNearbyCellsOf(player, grid_object_update, world_object_update);

		// Handle updates for creatures in combat with player and are more than 60 yards away
		if (player->isInCombat())
		{
			std::vector<Creature*> updateList;
			HostileReference* ref = player->getHostileRefManager().getFirst();

			while (ref)
			{
				if (Unit* unit = ref->getSource()->getOwner())
					if (unit->ToCreature() && unit->GetMapId() == player->GetMapId() && !unit->IsWithinDistInMap(player, GetVisibilityRange(), false))
						updateList.push_back(unit->ToCreature());

				ref = ref->next();
			}

			// Process deferred update list for player
			for (Creature* c : updateList)
				VisitNearbyCellsOf(c, grid_object_update, world_object_update);
		}
    }

    // non-player active objects, increasing iterator in the loop in case of object removal
    for (m_activeNonPlayersIter = m_activeNonPlayers.begin(); m_activeNonPlayersIter != m_activeNonPlayers.end();)
    {
        WorldObject* obj = *m_activeNonPlayersIter;
        ++m_activeNonPlayersIter;

        if (!obj || !obj->IsInWorld())
            continue;

        VisitNearbyCellsOf(obj, grid_object_update, world_object_update);
    }

    for (_transportsGameObjectUpdateIter = _transportsGameObject.begin(); _transportsGameObjectUpdateIter != _transportsGameObject.end();)
    {
        GameObject* gameObj = *_transportsGameObjectUpdateIter;
        ++_transportsGameObjectUpdateIter;

        if (!gameObj->IsInWorld())
            continue;

        gameObj->Update(t_diff);
    }

    for (_transportsUpdateIter = _transports.begin(); _transportsUpdateIter != _transports.end();)
    {
        WorldObject* obj = *_transportsUpdateIter;
        ++_transportsUpdateIter;

        if (!obj->IsInWorld())
            continue;

        obj->Update(t_diff);
    }

    ///- Process necessary scripts
    if (!m_scriptSchedule.empty())
    {
        i_scriptLock = true;
        ScriptsProcess();
        i_scriptLock = false;
    }

    MoveAllCreaturesInMoveList();
    MoveAllGameObjectsInMoveList();

    sScriptMgr->OnMapUpdate(this, t_diff);

#ifdef CROSS
    SetUpdating(false);
#endif
}

void Map::RemovePlayerFromMap(Player* player, bool remove)
{
    player->RemoveFromWorld();
    SendRemoveTransports(player);
    sOutdoorPvPMgr->HandlePlayerLeaveMap(player, GetId());

    player->UpdateObjectVisibility(true);
    if (player->IsInGrid())
        player->RemoveFromGrid();
    else
        ASSERT(remove); //maybe deleted in logoutplayer when player is not in a map

    if (remove)
    {
        DeleteFromWorld(player);
        sScriptMgr->OnPlayerLeaveMap(this, player);
    }
}

template<class T>
void Map::RemoveFromMap(T *obj, bool remove)
{
    if (Creature* creature = obj->ToCreature())
        sWildBattlePetMgr->OnRemoveToMap(creature);

    obj->RemoveFromWorld();
    if (obj->isActiveObject())
        RemoveFromActive(obj);

    if (obj->ToGameObject() && obj->ToGameObject()->IsTransport())
    {
        // Send delete pkt to out of range players
        Map::PlayerList const& players = GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            if (itr->getSource()->GetDistance(*obj) < obj->GetVisibilityRange())
                continue;

            UpdateData transData(obj->GetMapId());
            WorldPacket pkt;
            obj->BuildOutOfRangeUpdateBlock(&transData);
            if (transData.BuildPacket(&pkt))
                itr->getSource()->GetSession()->SendPacket(&pkt);
        }

        if (_transportsGameObjectUpdateIter != _transportsGameObject.end())
        {
            TransportGameObjectContainer::iterator itr = _transportsGameObject.find(obj->ToGameObject());
            if (itr == _transportsGameObject.end())
                return;
            if (itr == _transportsGameObjectUpdateIter)
                ++_transportsGameObjectUpdateIter;
            _transportsGameObject.erase(itr);
        }
        else
            _transportsGameObject.erase(obj->ToGameObject());
    }

    obj->UpdateObjectVisibility(true);
    obj->RemoveFromGrid();

    obj->ResetMap();

    if (remove)
    {
        // if option set then object already saved at this moment
        if (!sWorld->getBoolConfig(CONFIG_SAVE_RESPAWN_TIME_IMMEDIATELY))
            obj->SaveRespawnTime();
        DeleteFromWorld(obj);
    }
}

template<>
void Map::RemoveFromMap(Transport* obj, bool remove)
{
    obj->RemoveFromWorld();

    if (_transportsUpdateIter != _transports.end())
    {
        TransportsContainer::iterator itr = _transports.find(obj);
        if (itr == _transports.end())
            return;
        if (itr == _transportsUpdateIter)
            ++_transportsUpdateIter;
        _transports.erase(itr);
    }
    else
        _transports.erase(obj);

    obj->ResetMap();

    if (remove)
    {
        // if option set then object already saved at this moment
        if (!sWorld->getBoolConfig(CONFIG_SAVE_RESPAWN_TIME_IMMEDIATELY))
            obj->SaveRespawnTime();
        DeleteFromWorld(obj);
    }
}

void Map::PlayerRelocation(Player* player, float x, float y, float z, float orientation)
{
    ASSERT(player);

    Cell old_cell(player->GetPositionX(), player->GetPositionY());
    Cell new_cell(x, y);

    //! If hovering, always increase our server-side Z position
    //! Client automatically projects correct position based on Z coord sent in monster move
    //! and UNIT_FIELD_HOVER_HEIGHT sent in object updates
    if (player->HasUnitMovementFlag(MOVEMENTFLAG_HOVER))
        z += player->GetFloatValue(UNIT_FIELD_HOVER_HEIGHT);

    player->Relocate(x, y, z, orientation);
    player->m_movementInfo.pos.Relocate(x, y, z, orientation);

    if (player->IsVehicle())
        player->GetVehicleKit()->RelocatePassengers();

    if (old_cell.DiffGrid(new_cell) || old_cell.DiffCell(new_cell))
    {
        TC_LOG_DEBUG("maps", "Player %s relocation grid[%u, %u]cell[%u, %u]->grid[%u, %u]cell[%u, %u]", player->GetName(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());

        player->RemoveFromGrid();

        if (old_cell.DiffGrid(new_cell))
            EnsureGridLoadedForActiveObject(new_cell, player);

        AddToGrid(player, new_cell);
    }

    player->UpdateObjectVisibility(false);
}

void Map::CreatureRelocation(Creature* creature, float x, float y, float z, float ang, bool respawnRelocationOnFail)
{
    ASSERT(CheckGridIntegrity(creature, false));

    Cell old_cell = creature->GetCurrentCell();
    Cell new_cell(x, y);

    if (!respawnRelocationOnFail && !getNGrid(new_cell.GridX(), new_cell.GridY()))
        return;

    //! If hovering, always increase our server-side Z position
    //! Client automatically projects correct position based on Z coord sent in monster move
    //! and UNIT_FIELD_HOVER_HEIGHT sent in object updates
    if (creature->HasUnitMovementFlag(MOVEMENTFLAG_HOVER))
        z += creature->GetFloatValue(UNIT_FIELD_HOVER_HEIGHT);

    // delay creature move for grid/cell to grid/cell moves
    if (old_cell.DiffCell(new_cell) || old_cell.DiffGrid(new_cell))
    {
        #ifdef TRINITY_DEBUG
            TC_LOG_DEBUG("maps", "Creature (GUID: %u Entry: %u) added to moving list from grid[%u, %u]cell[%u, %u] to grid[%u, %u]cell[%u, %u].", creature->GetGUIDLow(), creature->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());
        #endif
        AddCreatureToMoveList(creature, x, y, z, ang);
        // in diffcell/diffgrid case notifiers called at finishing move creature in Map::MoveAllCreaturesInMoveList
    }
    else
    {
        creature->Relocate(x, y, z, ang);
        if (creature->IsVehicle())
            creature->GetVehicleKit()->RelocatePassengers();
        creature->UpdateObjectVisibility(false);
        RemoveCreatureFromMoveList(creature);
    }

    ASSERT(CheckGridIntegrity(creature, true));
}

void Map::GameObjectRelocation(GameObject* go, float x, float y, float z, float orientation, bool respawnRelocationOnFail)
{
    Cell integrity_check(go->GetPositionX(), go->GetPositionY());
    Cell old_cell = go->GetCurrentCell();

    ASSERT(integrity_check == old_cell);
    Cell new_cell(x, y);

    if (!respawnRelocationOnFail && !getNGrid(new_cell.GridX(), new_cell.GridY()))
        return;

    // delay creature move for grid/cell to grid/cell moves
    if (old_cell.DiffCell(new_cell) || old_cell.DiffGrid(new_cell))
    {
#ifdef TRINITY_DEBUG
        TC_LOG_DEBUG("maps", "GameObject (GUID: %u Entry: %u) added to moving list from grid[%u, %u]cell[%u, %u] to grid[%u, %u]cell[%u, %u].", go->GetGUIDLow(), go->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());
#endif
        AddGameObjectToMoveList(go, x, y, z, orientation);
        // in diffcell/diffgrid case notifiers called at finishing move go in Map::MoveAllGameObjectsInMoveList
    }
    else
    {
        go->Relocate(x, y, z, orientation);
        go->UpdateModelPosition();
        go->UpdateObjectVisibility(false);
        RemoveGameObjectFromMoveList(go);
    }

    old_cell = go->GetCurrentCell();
    integrity_check = Cell(go->GetPositionX(), go->GetPositionY());
    ASSERT(integrity_check == old_cell);
}

void Map::AddCreatureToMoveList(Creature* c, float x, float y, float z, float ang)
{
    if (_creatureToMoveLock) //can this happen?
        return;

    if (c->_moveState == MAP_OBJECT_CELL_MOVE_NONE)
        _creaturesToMove.push_back(c);
    c->SetNewCellPosition(x, y, z, ang);
}

void Map::RemoveCreatureFromMoveList(Creature* p_Creature, bool p_Force)
{
    if (_creatureToMoveLock) //can this happen?
        return;

    if (p_Creature->_moveState == MAP_OBJECT_CELL_MOVE_ACTIVE)
        p_Creature->_moveState = MAP_OBJECT_CELL_MOVE_INACTIVE;

    if (p_Force)
    {
        for (uint32 l_I = 0; l_I < _creaturesToMove.size(); l_I++)
        {
            if (_creaturesToMove[l_I] == p_Creature)
            {
                _creaturesToMove[l_I] = nullptr;
                break;
            }
        }
    }
}

void Map::AddGameObjectToMoveList(GameObject* go, float x, float y, float z, float ang)
{
    if (_gameObjectsToMoveLock) //can this happen?
        return;

    if (go->_moveState == MAP_OBJECT_CELL_MOVE_NONE)
        _gameObjectsToMove.push_back(go);
    go->SetNewCellPosition(x, y, z, ang);
}

void Map::RemoveGameObjectFromMoveList(GameObject* go)
{
    if (_gameObjectsToMoveLock) //can this happen?
        return;

    if (go->_moveState == MAP_OBJECT_CELL_MOVE_ACTIVE)
        go->_moveState = MAP_OBJECT_CELL_MOVE_INACTIVE;
}

void Map::MoveAllCreaturesInMoveList()
{
    _creatureToMoveLock = true;
    for (std::vector<Creature*>::iterator itr = _creaturesToMove.begin(); itr != _creaturesToMove.end(); ++itr)
    {
        Creature* c = *itr;

        if (c == nullptr)
            continue;

        if (c->FindMap() != this) //pet is teleported to another map
            continue;

        if (c->_moveState != MAP_OBJECT_CELL_MOVE_ACTIVE)
        {
            c->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
            continue;
        }

        c->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
        if (!c->IsInWorld())
            continue;

        // do move or do move to respawn or remove creature if previous all fail
        if (CreatureCellRelocation(c, Cell(c->_newPosition.m_positionX, c->_newPosition.m_positionY)))
        {
            // update pos
            c->Relocate(c->_newPosition);
            //c->SendMovementFlagUpdate(); possible creature crash fix.
            c->UpdateObjectVisibility(false);
        }
        else
        {
            // if creature can't be move in new cell/grid (not loaded) move it to repawn cell/grid
            // creature coordinates will be updated and notifiers send
            if (!CreatureRespawnRelocation(c, false))
            {
                // ... or unload (if respawn grid also not loaded)
                #ifdef TRINITY_DEBUG
                    TC_LOG_DEBUG("maps", "Creature (GUID: %u Entry: %u) cannot be move to unloaded respawn grid.", c->GetGUIDLow(), c->GetEntry());
                #endif
                //AddObjectToRemoveList(Pet*) should only be called in Pet::Remove
                //This may happen when a player just logs in and a pet moves to a nearby unloaded cell
                //To avoid this, we can load nearby cells when player log in
                //But this check is always needed to ensure safety
                //TODO: pets will disappear if this is outside CreatureRespawnRelocation
                //need to check why pet is frequently relocated to an unloaded cell
                if (c->isPet())
                    c->ToPet()->Remove(PET_SLOT_OTHER_PET, true, c->ToPet()->m_Stampeded);
                else
                    AddObjectToRemoveList(c);
            }
        }
    }
    _creaturesToMove.clear();
    _creatureToMoveLock = false;
}

void Map::MoveAllGameObjectsInMoveList()
{
    _gameObjectsToMoveLock = true;
    for (std::vector<GameObject*>::iterator itr = _gameObjectsToMove.begin(); itr != _gameObjectsToMove.end(); ++itr)
    {
        GameObject* go = *itr;
        if (go->FindMap() != this) //transport is teleported to another map
            continue;

        if (go->_moveState != MAP_OBJECT_CELL_MOVE_ACTIVE)
        {
            go->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
            continue;
        }

        go->_moveState = MAP_OBJECT_CELL_MOVE_NONE;
        if (!go->IsInWorld())
            continue;

        // do move or do move to respawn or remove creature if previous all fail
        if (GameObjectCellRelocation(go, Cell(go->_newPosition.m_positionX, go->_newPosition.m_positionY)))
        {
            // update pos
            go->Relocate(go->_newPosition);
            go->UpdateModelPosition();
            go->UpdateObjectVisibility(false);
        }
        else
        {
            // if GameObject can't be move in new cell/grid (not loaded) move it to repawn cell/grid
            // GameObject coordinates will be updated and notifiers send
            if (!GameObjectRespawnRelocation(go, false))
            {
                // ... or unload (if respawn grid also not loaded)
#ifdef TRINITY_DEBUG
                TC_LOG_DEBUG("maps", "GameObject (GUID: %u Entry: %u) cannot be move to unloaded respawn grid.", go->GetGUIDLow(), go->GetEntry());
#endif
                AddObjectToRemoveList(go);
            }
        }
    }
    _gameObjectsToMove.clear();
    _gameObjectsToMoveLock = false;
}

bool Map::CreatureCellRelocation(Creature* c, Cell new_cell)
{
    Cell const& old_cell = c->GetCurrentCell();
    if (!old_cell.DiffGrid(new_cell))                       // in same grid
    {
        // if in same cell then none do
        if (old_cell.DiffCell(new_cell))
        {
            #ifdef TRINITY_DEBUG
                TC_LOG_DEBUG("maps", "Creature (GUID: %u Entry: %u) moved in grid[%u, %u] from cell[%u, %u] to cell[%u, %u].", c->GetGUIDLow(), c->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.CellX(), new_cell.CellY());
            #endif

            c->RemoveFromGrid();
            AddToGrid(c, new_cell);
        }
        else
        {
            #ifdef TRINITY_DEBUG
                TC_LOG_DEBUG("maps", "Creature (GUID: %u Entry: %u) moved in same grid[%u, %u]cell[%u, %u].", c->GetGUIDLow(), c->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY());
            #endif
        }

        return true;
    }

    // in diff. grids but active creature
    if (c->isActiveObject())
    {
        EnsureGridLoadedForActiveObject(new_cell, c);

        #ifdef TRINITY_DEBUG
            TC_LOG_DEBUG("maps", "Active creature (GUID: %u Entry: %u) moved from grid[%u, %u]cell[%u, %u] to grid[%u, %u]cell[%u, %u].", c->GetGUIDLow(), c->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());
        #endif

        c->RemoveFromGrid();
        AddToGrid(c, new_cell);

        return true;
    }

    // in diff. loaded grid normal creature
    if (IsGridLoaded(GridCoord(new_cell.GridX(), new_cell.GridY())))
    {
        #ifdef TRINITY_DEBUG
            TC_LOG_DEBUG("maps", "Creature (GUID: %u Entry: %u) moved from grid[%u, %u]cell[%u, %u] to grid[%u, %u]cell[%u, %u].", c->GetGUIDLow(), c->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());
        #endif

        c->RemoveFromGrid();
        EnsureGridCreated(GridCoord(new_cell.GridX(), new_cell.GridY()));
        AddToGrid(c, new_cell);

        return true;
    }

    // fail to move: normal creature attempt move to unloaded grid
    #ifdef TRINITY_DEBUG
        TC_LOG_DEBUG("maps", "Creature (GUID: %u Entry: %u) attempted to move from grid[%u, %u]cell[%u, %u] to unloaded grid[%u, %u]cell[%u, %u].", c->GetGUIDLow(), c->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());
    #endif
    return false;
}

bool Map::GameObjectCellRelocation(GameObject* go, Cell new_cell)
{
    Cell const& old_cell = go->GetCurrentCell();
    if (!old_cell.DiffGrid(new_cell))                       // in same grid
    {
        // if in same cell then none do
        if (old_cell.DiffCell(new_cell))
        {
            #ifdef TRINITY_DEBUG
                TC_LOG_DEBUG("maps", "GameObject (GUID: %u Entry: %u) moved in grid[%u, %u] from cell[%u, %u] to cell[%u, %u].", go->GetGUIDLow(), go->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.CellX(), new_cell.CellY());
            #endif

            go->RemoveFromGrid();
            AddToGrid(go, new_cell);
        }
        else
        {
            #ifdef TRINITY_DEBUG
                TC_LOG_DEBUG("maps", "GameObject (GUID: %u Entry: %u) moved in same grid[%u, %u]cell[%u, %u].", go->GetGUIDLow(), go->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY());
            #endif
        }

        return true;
    }

    // in diff. grids but active GameObject
    if (go->isActiveObject())
    {
        EnsureGridLoadedForActiveObject(new_cell, go);

        #ifdef TRINITY_DEBUG
            TC_LOG_DEBUG("maps", "Active GameObject (GUID: %u Entry: %u) moved from grid[%u, %u]cell[%u, %u] to grid[%u, %u]cell[%u, %u].", go->GetGUIDLow(), go->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());
        #endif

        go->RemoveFromGrid();
        AddToGrid(go, new_cell);

        return true;
    }

    // in diff. loaded grid normal GameObject
    if (IsGridLoaded(GridCoord(new_cell.GridX(), new_cell.GridY())))
    {
        #ifdef TRINITY_DEBUG
            TC_LOG_DEBUG("maps", "GameObject (GUID: %u Entry: %u) moved from grid[%u, %u]cell[%u, %u] to grid[%u, %u]cell[%u, %u].", go->GetGUIDLow(), go->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());
        #endif

        go->RemoveFromGrid();
        EnsureGridCreated(GridCoord(new_cell.GridX(), new_cell.GridY()));
        AddToGrid(go, new_cell);

        return true;
    }

    // fail to move: normal GameObject attempt move to unloaded grid
    #ifdef TRINITY_DEBUG
        TC_LOG_DEBUG("maps", "GameObject (GUID: %u Entry: %u) attempted to move from grid[%u, %u]cell[%u, %u] to unloaded grid[%u, %u]cell[%u, %u].", go->GetGUIDLow(), go->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());
    #endif
    return false;
}

bool Map::CreatureRespawnRelocation(Creature* c, bool diffGridOnly)
{
    float resp_x, resp_y, resp_z, resp_o;
    c->GetRespawnPosition(resp_x, resp_y, resp_z, &resp_o);
    Cell resp_cell(resp_x, resp_y);

    //creature will be unloaded with grid
    if (diffGridOnly && !c->GetCurrentCell().DiffGrid(resp_cell))
        return true;

    c->CombatStop();
    c->GetMotionMaster()->Clear();

    #ifdef TRINITY_DEBUG
        TC_LOG_DEBUG("maps", "Creature (GUID: %u Entry: %u) moved from grid[%u, %u]cell[%u, %u] to respawn grid[%u, %u]cell[%u, %u].", c->GetGUIDLow(), c->GetEntry(), c->GetCurrentCell().GridX(), c->GetCurrentCell().GridY(), c->GetCurrentCell().CellX(), c->GetCurrentCell().CellY(), resp_cell.GridX(), resp_cell.GridY(), resp_cell.CellX(), resp_cell.CellY());
    #endif

    // teleport it to respawn point (like normal respawn if player see)
    if (CreatureCellRelocation(c, resp_cell))
    {
        c->Relocate(resp_x, resp_y, resp_z, resp_o);
        c->GetMotionMaster()->Initialize();                 // prevent possible problems with default move generators
        //CreatureRelocationNotify(c, resp_cell, resp_cell.GetCellCoord());
        c->UpdateObjectVisibility(false);
        return true;
    }

    return false;
}

bool Map::GameObjectRespawnRelocation(GameObject* go, bool diffGridOnly)
{
    float resp_x, resp_y, resp_z, resp_o;
    go->GetRespawnPosition(resp_x, resp_y, resp_z, &resp_o);
    Cell resp_cell(resp_x, resp_y);

    //GameObject will be unloaded with grid
    if (diffGridOnly && !go->GetCurrentCell().DiffGrid(resp_cell))
        return true;

    // teleport it to respawn point (like normal respawn if player see)
    if (GameObjectCellRelocation(go, resp_cell))
    {
        go->Relocate(resp_x, resp_y, resp_z, resp_o);
        go->UpdateObjectVisibility(false);
        return true;
    }

    return false;
}

bool Map::UnloadGrid(NGridType& ngrid, bool unloadAll)
{
    const uint32 x = ngrid.getX();
    const uint32 y = ngrid.getY();

    {
        if (!unloadAll)
        {
            //pets, possessed creatures (must be active), transport passengers
            if (ngrid.GetWorldObjectCountInNGrid<Creature>())
                return false;

            if (ActiveObjectsNearGrid(ngrid))
                return false;
        }

        TC_LOG_DEBUG("maps", "Unloading grid[%u, %u] for map %u", x, y, GetId());

        if (!unloadAll)
        {
            // Finish creature moves, remove and delete all creatures with delayed remove before moving to respawn grids
            // Must know real mob position before move
            MoveAllCreaturesInMoveList();
            MoveAllGameObjectsInMoveList();

            // move creatures to respawn grids if this is diff.grid or to remove list
            ObjectGridEvacuator worker;
            TypeContainerVisitor<ObjectGridEvacuator, GridTypeMapContainer> visitor(worker);
            ngrid.VisitAllGrids(visitor);

            // Finish creature moves, remove and delete all creatures with delayed remove before unload
            MoveAllCreaturesInMoveList();
            MoveAllGameObjectsInMoveList();
        }

        {
            ObjectGridCleaner worker;
            TypeContainerVisitor<ObjectGridCleaner, GridTypeMapContainer> visitor(worker);
            ngrid.VisitAllGrids(visitor);
        }

        RemoveAllObjectsInRemoveList();

        {
            ObjectGridUnloader worker;
            TypeContainerVisitor<ObjectGridUnloader, GridTypeMapContainer> visitor(worker);
            ngrid.VisitAllGrids(visitor);
        }

        ASSERT(i_objectsToRemove.empty());

        delete &ngrid;
        setNGrid(NULL, x, y);
    }
    int gx = (MAX_NUMBER_OF_GRIDS - 1) - x;
    int gy = (MAX_NUMBER_OF_GRIDS - 1) - y;

    // delete grid map, but don't delete if it is from parent map (and thus only reference)
    //+++if (GridMaps[gx][gy]) don't check for GridMaps[gx][gy], we might have to unload vmaps
    {
        if (i_InstanceId == 0)
        {
            if (GridMaps[gx][gy])
            {
                GridMaps[gx][gy]->unloadData();
                delete GridMaps[gx][gy];
            }

            VMAP::VMapFactory::createOrGetVMapManager()->unloadMap(GetId(), gx, gy);
            MMAP::MMapFactory::createOrGetMMapManager()->unloadMap(GetId(), gx, gy);
        }
        else
            ((MapInstanced*)m_parentMap)->RemoveGridMapReference(GridCoord(gx, gy));

        GridMaps[gx][gy] = NULL;
    }
    TC_LOG_DEBUG("maps", "Unloading grid[%u, %u] for map %u finished", x, y, GetId());
    return true;
}

void Map::RemoveAllPlayers()
{
    if (HavePlayers())
    {
        for (MapRefManager::iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
        {
            Player* player = itr->getSource();
            if (!player->IsBeingTeleportedFar())
            {
                // this is happening for bg
                TC_LOG_ERROR("maps", "Map::UnloadAll: player %s is still in map %u during unload, this should not happen!", player->GetName(), GetId());
                player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ, player->GetOrientation());
            }
        }
    }
}

void Map::UnloadAll()
{
    // clear all delayed moves, useless anyway do this moves before map unload.
    _creaturesToMove.clear();
    _gameObjectsToMove.clear();

    for (GridRefManager<NGridType>::iterator i = GridRefManager<NGridType>::begin(); i != GridRefManager<NGridType>::end();)
    {
        NGridType &grid(*i->getSource());
        ++i;
        UnloadGrid(grid, true);       // deletes the grid and removes it from the GridRefManager
    }
}

inline GridMap* Map::GetGrid(float x, float y)
{
    // half opt method
    int gx = (int)(CENTER_GRID_ID - x / SIZE_OF_GRIDS);                       //grid x
    int gy = (int)(CENTER_GRID_ID - y / SIZE_OF_GRIDS);                       //grid y

    if (gx >= MAX_NUMBER_OF_GRIDS || gy >= MAX_NUMBER_OF_GRIDS ||
        gx < 0 || gy < 0)
        return NULL;

    // ensure GridMap is loaded
    EnsureGridCreated(GridCoord((MAX_NUMBER_OF_GRIDS - 1) - gx, (MAX_NUMBER_OF_GRIDS - 1) - gy));

    return GridMaps[gx][gy];
}

float Map::GetWaterOrGroundLevel(float x, float y, float z, float* ground /*= NULL*/, bool /*swim = false*/) const
{
    if (const_cast<Map*>(this)->GetGrid(x, y))
    {
        // we need ground level (including grid height version) for proper return water level in point
        float ground_z = GetHeight(PHASEMASK_NORMAL, x, y, z, true, 50.0f);
        if (ground)
            *ground = ground_z;

        LiquidData liquid_status;

        ZLiquidStatus res = getLiquidStatus(x, y, ground_z, MAP_ALL_LIQUIDS, &liquid_status);
        return res ? liquid_status.level : ground_z;
    }

    return VMAP_INVALID_HEIGHT_VALUE;
}

float Map::GetHeight(float x, float y, float z, bool checkVMap /*= true*/, float maxSearchDist /*= DEFAULT_HEIGHT_SEARCH*/) const
{
    // find raw .map surface under Z coordinates
    float mapHeight = VMAP_INVALID_HEIGHT_VALUE;
    if (GridMap* gmap = const_cast<Map*>(this)->GetGrid(x, y))
    {
        float gridHeight = gmap->getHeight(x, y);
        // look from a bit higher pos to find the floor, ignore under surface case
        if (z + 2.0f > gridHeight)
            mapHeight = gridHeight;
    }

    float vmapHeight = VMAP_INVALID_HEIGHT_VALUE;
    if (checkVMap)
    {
        VMAP::IVMapManager* vmgr = VMAP::VMapFactory::createOrGetVMapManager();
        if (vmgr->isHeightCalcEnabled())
            vmapHeight = vmgr->getHeight(GetId(), x, y, z + 2.0f, maxSearchDist);   // look from a bit higher pos to find the floor
    }

    // mapHeight set for any above raw ground Z or <= INVALID_HEIGHT
    // vmapheight set for any under Z value or <= INVALID_HEIGHT
    if (vmapHeight > INVALID_HEIGHT)
    {
        if (mapHeight > INVALID_HEIGHT)
        {
            // we have mapheight and vmapheight and must select more appropriate

            // we are already under the surface or vmap height above map heigt
            // or if the distance of the vmap height is less the land height distance
            if (vmapHeight > mapHeight || std::fabs(mapHeight - z) > std::fabs(vmapHeight - z))
                return vmapHeight;
            else
                return mapHeight;                           // better use .map surface height
        }
        else
            return vmapHeight;                              // we have only vmapHeight (if have)
    }

    return mapHeight;                               // explicitly use map data
}

float Map::GetMinHeight(float x, float y) const
{
    if (GridMap const* grid = const_cast<Map*>(this)->GetGrid(x, y))
        return grid->getMinHeight(x, y);

    return -500.0f;
}

inline bool IsOutdoorWMO(uint32 mogpFlags, int32 /*adtId*/, int32 /*rootId*/, int32 /*groupId*/, WMOAreaTableEntry const* wmoEntry, AreaTableEntry const* atEntry)
{
    bool outdoor = true;

    if (wmoEntry && atEntry)
    {
        if (atEntry->Flags & AREA_FLAG_OUTSIDE)
            return true;
        if (atEntry->Flags & AREA_FLAG_INSIDE)
            return false;
    }

    outdoor = mogpFlags&0x8;

    if (wmoEntry)
    {
        if (wmoEntry->Flags & 4)
            return true;
        if ((wmoEntry->Flags & 2)!=0)
            outdoor = false;
    }
    return outdoor;
}

bool Map::IsOutdoors(float x, float y, float z) const
{
    uint32 mogpFlags;
    int32 adtId, rootId, groupId;

    // no wmo found? -> outside by default
    if (!GetAreaInfo(x, y, z, mogpFlags, adtId, rootId, groupId))
        return true;

    AreaTableEntry const* atEntry = 0;
    WMOAreaTableEntry const* wmoEntry= GetWMOAreaTableEntryByTripple(rootId, adtId, groupId);
    if (wmoEntry)
    {
        TC_LOG_DEBUG("maps", "Got WMOAreaTableEntry! flag %u, areaid %u", wmoEntry->Flags, wmoEntry->areaId);
        atEntry = GetAreaEntryByAreaID(wmoEntry->areaId);
    }
    return IsOutdoorWMO(mogpFlags, adtId, rootId, groupId, wmoEntry, atEntry);
}

bool Map::GetAreaInfo(float x, float y, float z, uint32 &flags, int32 &adtId, int32 &rootId, int32 &groupId) const
{
    float vmap_z = z;
    VMAP::IVMapManager* vmgr = VMAP::VMapFactory::createOrGetVMapManager();
    if (vmgr->getAreaInfo(GetId(), x, y, vmap_z, flags, adtId, rootId, groupId))
    {
        // check if there's terrain between player height and object height
        if (GridMap* gmap = const_cast<Map*>(this)->GetGrid(x, y))
        {
            float _mapheight = gmap->getHeight(x, y);
            // z + 2.0f condition taken from GetHeight(), not sure if it's such a great choice...
            if (z + 2.0f > _mapheight &&  _mapheight > vmap_z)
                return false;
        }
        return true;
    }
    return false;
}

uint16 Map::GetAreaFlag(float x, float y, float z, bool *isOutdoors) const
{
    uint32 mogpFlags;
    int32 adtId, rootId, groupId;
    WMOAreaTableEntry const* wmoEntry = 0;
    AreaTableEntry const* atEntry = 0;
    bool haveAreaInfo = false;

    if (GetAreaInfo(x, y, z, mogpFlags, adtId, rootId, groupId))
    {
        haveAreaInfo = true;
        wmoEntry = GetWMOAreaTableEntryByTripple(rootId, adtId, groupId);
        if (wmoEntry)
            atEntry = GetAreaEntryByAreaID(wmoEntry->areaId);
    }

    uint16 areaflag;

    if (atEntry)
        areaflag = atEntry->AreaBit;
    else
    {
        if (GridMap* gmap = const_cast<Map*>(this)->GetGrid(x, y))
            areaflag = gmap->getArea(x, y);
        // this used while not all *.map files generated (instances)
        else
            areaflag = GetAreaFlagByMapId(i_mapEntry->MapID);
    }

    if (isOutdoors)
    {
        if (haveAreaInfo)
            *isOutdoors = IsOutdoorWMO(mogpFlags, adtId, rootId, groupId, wmoEntry, atEntry);
        else
            *isOutdoors = true;
    }
    return areaflag;
 }

uint8 Map::GetTerrainType(float x, float y) const
{
    if (GridMap* gmap = const_cast<Map*>(this)->GetGrid(x, y))
        return gmap->getTerrainType(x, y);
    else
        return 0;
}

ZLiquidStatus Map::getLiquidStatus(float x, float y, float z, uint8 ReqLiquidType, LiquidData* data) const
{
    ZLiquidStatus result = LIQUID_MAP_NO_WATER;
    VMAP::IVMapManager* vmgr = VMAP::VMapFactory::createOrGetVMapManager();
    float liquid_level = INVALID_HEIGHT;
    float ground_level = INVALID_HEIGHT;
    uint32 liquid_type = 0;
    if (vmgr->GetLiquidLevel(GetId(), x, y, z, ReqLiquidType, liquid_level, ground_level, liquid_type))
    {
        TC_LOG_DEBUG("maps", "getLiquidStatus(): vmap liquid level: %f ground: %f type: %u", liquid_level, ground_level, liquid_type);
        // Check water level and ground level
        if (liquid_level > ground_level && z > ground_level - 2)
        {
            // All ok in water -> store data
            if (data)
            {
                // hardcoded in client like this
                if (GetId() == 530 && liquid_type == 2)
                    liquid_type = 15;

                uint32 liquidFlagType = 0;
                if (LiquidTypeEntry const* liq = sLiquidTypeStore.LookupEntry(liquid_type))
                    liquidFlagType = liq->Type;

                if (liquid_type && liquid_type < 21)
                {
                    if (AreaTableEntry const* area = GetAreaEntryByAreaFlagAndMap(GetAreaFlag(x, y, z), GetId()))
                    {
                        uint32 overrideLiquid = area->LiquidTypeID[liquidFlagType];
                        if (!overrideLiquid && area->ParentAreaID)
                        {
                            area = GetAreaEntryByAreaID(area->ParentAreaID);
                            if (area)
                                overrideLiquid = area->LiquidTypeID[liquidFlagType];
                        }

                        if (LiquidTypeEntry const* liq = sLiquidTypeStore.LookupEntry(overrideLiquid))
                        {
                            liquid_type = overrideLiquid;
                            liquidFlagType = liq->Type;
                        }
                    }
                }

                data->level = liquid_level;
                data->depth_level = ground_level;

                data->entry = liquid_type;
                data->type_flags = 1 << liquidFlagType;
            }

            float delta = liquid_level - z;

            // Get position delta
            if (delta > 2.0f)                   // Under water
                return LIQUID_MAP_UNDER_WATER;
            if (delta > 0.0f)                   // In water
                return LIQUID_MAP_IN_WATER;
            if (delta > -0.1f)                   // Walk on water
                return LIQUID_MAP_WATER_WALK;
            result = LIQUID_MAP_ABOVE_WATER;
        }
    }

    if (GridMap* gmap = const_cast<Map*>(this)->GetGrid(x, y))
    {
        LiquidData map_data;
        ZLiquidStatus map_result = gmap->getLiquidStatus(x, y, z, ReqLiquidType, &map_data);
        // Not override LIQUID_MAP_ABOVE_WATER with LIQUID_MAP_NO_WATER:
        if (map_result != LIQUID_MAP_NO_WATER && (map_data.level > ground_level))
        {
            if (data)
            {
                // hardcoded in client like this
                if (GetId() == 530 && map_data.entry == 2)
                    map_data.entry = 15;

                *data = map_data;
            }
            return map_result;
        }
    }
    return result;
}

float Map::GetWaterLevel(float x, float y) const
{
    if (GridMap* gmap = const_cast<Map*>(this)->GetGrid(x, y))
        return gmap->getLiquidLevel(x, y);
    else
        return 0;
}

uint32 Map::GetAreaIdByAreaFlag(uint16 areaflag, uint32 map_id)
{
    AreaTableEntry const* entry = GetAreaEntryByAreaFlagAndMap(areaflag, map_id);

    if (entry)
        return entry->ID;
    else
        return 0;
}

uint32 Map::GetZoneIdByAreaFlag(uint16 areaflag, uint32 map_id)
{
    AreaTableEntry const* entry = GetAreaEntryByAreaFlagAndMap(areaflag, map_id);

    if (entry)
        return (entry->ParentAreaID != 0) ? entry->ParentAreaID : entry->ID;
    else
        return 0;
}

void Map::GetZoneAndAreaIdByAreaFlag(uint32& zoneid, uint32& areaid, uint16 areaflag, uint32 map_id)
{
    AreaTableEntry const* entry = GetAreaEntryByAreaFlagAndMap(areaflag, map_id);

    areaid = entry ? entry->ID : 0;
    zoneid = entry ? ((entry->ParentAreaID != 0) ? entry->ParentAreaID : entry->ID) : 0;
}

bool Map::isInLineOfSight(float x1, float y1, float z1, float x2, float y2, float z2, uint32 phasemask) const
{
    return VMAP::VMapFactory::createOrGetVMapManager()->isInLineOfSight(GetId(), x1, y1, z1, x2, y2, z2)
        && _dynamicTree.isInLineOfSight(x1, y1, z1, x2, y2, z2, phasemask);
}

bool Map::getObjectHitPos(uint32 phasemask, float x1, float y1, float z1, float x2, float y2, float z2, float& rx, float& ry, float& rz, float modifyDist)
{
    G3D::Vector3 startPos = G3D::Vector3(x1, y1, z1);
    G3D::Vector3 dstPos = G3D::Vector3(x2, y2, z2);

    G3D::Vector3 resultPos;
    bool result = _dynamicTree.getObjectHitPos(phasemask, startPos, dstPos, resultPos, modifyDist);

    rx = resultPos.x;
    ry = resultPos.y;
    rz = resultPos.z;
    return result;
}

float Map::GetHeight(uint32 phasemask, float x, float y, float z, bool vmap/*=true*/, float maxSearchDist/*=DEFAULT_HEIGHT_SEARCH*/) const
{
    return std::max<float>(GetHeight(x, y, z, vmap, maxSearchDist), _dynamicTree.getHeight(x, y, z, maxSearchDist, phasemask));
}

bool Map::IsInWater(float x, float y, float pZ, LiquidData* data) const
{
    // Check surface in x, y point for liquid
    if (const_cast<Map*>(this)->GetGrid(x, y))
    {
        LiquidData liquid_status;
        LiquidData* liquid_ptr = data ? data : &liquid_status;
        if (getLiquidStatus(x, y, pZ, MAP_ALL_LIQUIDS, liquid_ptr))
            return true;
    }
    return false;
}

bool Map::IsUnderWater(float x, float y, float z) const
{
    if (const_cast<Map*>(this)->GetGrid(x, y))
    {
        if (getLiquidStatus(x, y, z, MAP_LIQUID_TYPE_WATER|MAP_LIQUID_TYPE_OCEAN)&LIQUID_MAP_UNDER_WATER)
            return true;
    }
    return false;
}

bool Map::CheckGridIntegrity(Creature* c, bool moved) const
{
    Cell const& cur_cell = c->GetCurrentCell();
    Cell xy_cell(c->GetPositionX(), c->GetPositionY());
    if (xy_cell != cur_cell)
    {
        TC_LOG_DEBUG("maps", "Creature (GUID: %u) X: %f Y: %f (%s) is in grid[%u, %u]cell[%u, %u] instead of grid[%u, %u]cell[%u, %u]",
            c->GetGUIDLow(),
            c->GetPositionX(), c->GetPositionY(), (moved ? "final" : "original"),
            cur_cell.GridX(), cur_cell.GridY(), cur_cell.CellX(), cur_cell.CellY(),
            xy_cell.GridX(),  xy_cell.GridY(),  xy_cell.CellX(),  xy_cell.CellY());
        return true;                                        // not crash at error, just output error in debug mode
    }

    return true;
}

char const* Map::GetMapName() const
{
    return i_mapEntry ? i_mapEntry->MapNameLang : "UNNAMEDMAP\x0";
}

void Map::UpdateObjectVisibility(WorldObject* obj, Cell cell, CellCoord cellpair)
{
    cell.SetNoCreate();
    Trinity::VisibleChangesNotifier notifier(*obj);
    TypeContainerVisitor<Trinity::VisibleChangesNotifier, WorldTypeMapContainer > player_notifier(notifier);
    cell.Visit(cellpair, player_notifier, *this, *obj, obj->GetVisibilityRange());
}

void Map::UpdateObjectsVisibilityFor(Player* player, Cell cell, CellCoord cellpair)
{
    Trinity::VisibleNotifier notifier(*player);

    cell.SetNoCreate();
    TypeContainerVisitor<Trinity::VisibleNotifier, WorldTypeMapContainer > world_notifier(notifier);
    TypeContainerVisitor<Trinity::VisibleNotifier, GridTypeMapContainer  > grid_notifier(notifier);
    cell.Visit(cellpair, world_notifier, *this, *player, player->GetSightRange());
    cell.Visit(cellpair, grid_notifier,  *this, *player, player->GetSightRange());

    // send data
    notifier.SendToSelf();
}

void Map::SendInitSelf(Player* p_Player, bool p_Switched)
{
    UpdateData l_Data(p_Player->GetMapId());

    // attach to player data current transport data
    if (Transport* l_Transport = p_Player->GetTransport())
        l_Transport->BuildCreateUpdateBlockForPlayer(&l_Data, p_Player);

    // build data for self presence in world at own client (one time for map)
    if (!p_Switched)
        p_Player->BuildCreateUpdateBlockForPlayer(&l_Data, p_Player);
    else ///< Blizzard doesn't send create when switch to phased map (garrisson, ashran...)
    {
        p_Player->ForceValuesUpdateAtIndex(OBJECT_FIELD_ENTRY_ID);
        p_Player->ForceValuesUpdateAtIndex(UNIT_FIELD_DISPLAY_ID);
        p_Player->ForceValuesUpdateAtIndex(UNIT_FIELD_LEVEL);

        p_Player->BuildValuesUpdateBlockForPlayer(&l_Data, p_Player);
    }

    // build other passengers at transport also (they always visible and marked as visible and will not send at visibility update at add to map
    if (Transport* l_Transport = p_Player->GetTransport())
    {
        for (std::set<WorldObject*>::const_iterator l_Iter = l_Transport->GetPassengers().begin(); l_Iter != l_Transport->GetPassengers().end(); ++l_Iter)
        {
            if (p_Player != (*l_Iter) && p_Player->HaveAtClient(*l_Iter))
                (*l_Iter)->BuildCreateUpdateBlockForPlayer(&l_Data, p_Player);
        }
    }

    WorldPacket l_Packet;
    if (l_Data.BuildPacket(&l_Packet))
        p_Player->GetSession()->SendPacket(&l_Packet);
}

/// Hack to send out transports
void Map::SendInitTransports(Player* player)
{
    UpdateData transData(player->GetMapId());

    // GAMEOBJECT_TYPE_MO_TRANSPORT
    for (TransportsContainer::const_iterator i = _transports.begin(); i != _transports.end(); ++i)
    {
        if (*i != player->GetTransport())
            (*i)->BuildCreateUpdateBlockForPlayer(&transData, player);
    }

    // GAMEOBJECT_TYPE_TRANSPORT
    for (TransportGameObjectContainer::const_iterator i = _transportsGameObject.begin(); i != _transportsGameObject.end(); ++i)
        (*i)->BuildCreateUpdateBlockForPlayer(&transData, player);

    WorldPacket packet;
    if (transData.BuildPacket(&packet))
        player->GetSession()->SendPacket(&packet);
}

void Map::SendRemoveTransports(Player* player)
{
    UpdateData transData(player->GetMapId());

    // GAMEOBJECT_TYPE_MO_TRANSPORT
    for (TransportsContainer::const_iterator i = _transports.begin(); i != _transports.end(); ++i)
    {
        if (*i != player->GetTransport())
            (*i)->BuildOutOfRangeUpdateBlock(&transData);
    }

    // GAMEOBJECT_TYPE_TRANSPORT
    for (TransportGameObjectContainer::const_iterator i = _transportsGameObject.begin(); i != _transportsGameObject.end(); ++i)
        (*i)->BuildOutOfRangeUpdateBlock(&transData);

    WorldPacket packet;
    if (transData.BuildPacket(&packet))
        player->GetSession()->SendPacket(&packet);
}

inline void Map::setNGrid(NGridType *grid, uint32 x, uint32 y)
{
    if (x >= MAX_NUMBER_OF_GRIDS || y >= MAX_NUMBER_OF_GRIDS)
    {
        TC_LOG_ERROR("maps", "map::setNGrid() Invalid grid coordinates found: %d, %d!", x, y);
        ABORT();
    }
    i_grids[x][y] = grid;
}

void Map::DelayedUpdate(const uint32 t_diff)
{
    RemoveAllObjectsInRemoveList();

    // Don't unload grids if it's battleground, since we may have manually added GOs, creatures, those doesn't load from DB at grid re-load !
    // This isn't really bother us, since as soon as we have instanced BG-s, the whole map unloads as the BG gets ended
    if (!IsBattlegroundOrArena())
    {
        for (GridRefManager<NGridType>::iterator i = GridRefManager<NGridType>::begin(); i != GridRefManager<NGridType>::end();)
        {
            NGridType *grid = i->getSource();
            GridInfo* info = i->getSource()->getGridInfoRef();
            ++i;                                                // The update might delete the map and we need the next map before the iterator gets invalid
            ASSERT(grid->GetGridState() >= 0 && grid->GetGridState() < MAX_GRID_STATE);
            si_GridStates[grid->GetGridState()]->Update(*this, *grid, *info, t_diff);
        }
    }
}

void Map::AddObjectToRemoveList(WorldObject* obj)
{
    ASSERT(obj->GetMapId() == GetId() && obj->GetInstanceId() == GetInstanceId());

    obj->CleanupsBeforeDelete(false);                            // remove or simplify at least cross referenced links

    i_objectsToRemove.insert(obj);
    //TC_LOG_DEBUG("maps", "Object (GUID: %u TypeId: %u) added to removing list.", obj->GetGUIDLow(), obj->GetTypeId());
}

void Map::AddObjectToSwitchList(WorldObject* obj, bool on)
{
    ASSERT(obj->GetMapId() == GetId() && obj->GetInstanceId() == GetInstanceId());
    // i_objectsToSwitch is iterated only in Map::RemoveAllObjectsInRemoveList() and it uses
    // the contained objects only if GetTypeId() == TYPEID_UNIT , so we can return in all other cases
    if (obj->GetTypeId() != TYPEID_UNIT)
        return;

    std::map<WorldObject*, bool>::iterator itr = i_objectsToSwitch.find(obj);
    if (itr == i_objectsToSwitch.end())
        i_objectsToSwitch.insert(itr, std::make_pair(obj, on));
    else if (itr->second != on)
        i_objectsToSwitch.erase(itr);
    else
        ABORT();
}

void Map::RemoveAllObjectsInRemoveList()
{
    while (!i_objectsToSwitch.empty())
    {
        std::map<WorldObject*, bool>::iterator itr = i_objectsToSwitch.begin();
        WorldObject* obj = itr->first;
        bool on = itr->second;
        i_objectsToSwitch.erase(itr);

        if (obj->GetTypeId() == TYPEID_UNIT || obj->GetTypeId() == TYPEID_GAMEOBJECT && !obj->IsPermanentWorldObject())
            SwitchGridContainers(obj, on);
    }

    //TC_LOG_DEBUG("maps", "Object remover 1 check.");
    while (!i_objectsToRemove.empty())
    {
        std::set<WorldObject*>::iterator itr = i_objectsToRemove.begin();
        WorldObject* obj = *itr;

        if (!obj)
            continue;

        switch (obj->GetTypeId())
        {
            case TYPEID_CORPSE:
            {
                Corpse* corpse = ObjectAccessor::GetCorpse(*obj, obj->GetGUID());
                if (!corpse)
                    TC_LOG_ERROR("maps", "Tried to delete corpse/bones %u that is not in map.", obj->GetGUIDLow());
                else
                    RemoveFromMap(corpse, true);
                break;
            }
        case TYPEID_DYNAMICOBJECT:
            RemoveFromMap((DynamicObject*)obj, true);
            break;
        case TYPEID_AREATRIGGER:
            RemoveFromMap((AreaTrigger*)obj, true);
            break;
        case TYPEID_CONVERSATION:
            RemoveFromMap((Conversation*)obj, true);
            break;
        case TYPEID_GAMEOBJECT:
            RemoveFromMap((GameObject*)obj, true);
            break;
        case TYPEID_UNIT:
            // in case triggered sequence some spell can continue casting after prev CleanupsBeforeDelete call
            // make sure that like sources auras/etc removed before destructor start
            obj->ToCreature()->CleanupsBeforeDelete();
            RemoveFromMap(obj->ToCreature(), true);
            break;
        default:
            TC_LOG_ERROR("maps", "Non-grid object (TypeId: %u) is in grid object remove list, ignored.", obj->GetTypeId());
            break;
        }

        i_objectsToRemove.erase(itr);
    }

    //TC_LOG_DEBUG("maps", "Object remover 2 check.");
}

uint32 Map::GetPlayersCountExceptGMs() const
{
    uint32 count = 0;
    for (MapRefManager::const_iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
        if (!itr->getSource()->isGameMaster())
            ++count;
    return count;
}

void Map::SendToPlayers(WorldPacket const* data) const
{
    for (MapRefManager::const_iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
        itr->getSource()->GetSession()->SendPacket(data);
}

bool Map::ActiveObjectsNearGrid(NGridType const& ngrid) const
{
    CellCoord cell_min(ngrid.getX() * MAX_NUMBER_OF_CELLS, ngrid.getY() * MAX_NUMBER_OF_CELLS);
    CellCoord cell_max(cell_min.x_coord + MAX_NUMBER_OF_CELLS, cell_min.y_coord+MAX_NUMBER_OF_CELLS);

    //we must find visible range in cells so we unload only non-visible cells...
    float viewDist = GetVisibilityRange();
    int cell_range = (int)ceilf(viewDist / SIZE_OF_GRID_CELL) + 1;

    cell_min.dec_x(cell_range);
    cell_min.dec_y(cell_range);
    cell_max.inc_x(cell_range);
    cell_max.inc_y(cell_range);

    for (MapRefManager::const_iterator iter = m_mapRefManager.begin(); iter != m_mapRefManager.end(); ++iter)
    {
        Player* player = iter->getSource();

        CellCoord p = Trinity::ComputeCellCoord(player->GetPositionX(), player->GetPositionY());
        if ((cell_min.x_coord <= p.x_coord && p.x_coord <= cell_max.x_coord) &&
            (cell_min.y_coord <= p.y_coord && p.y_coord <= cell_max.y_coord))
            return true;
    }

    for (ActiveNonPlayers::const_iterator iter = m_activeNonPlayers.begin(); iter != m_activeNonPlayers.end(); ++iter)
    {
        WorldObject* obj = *iter;

        CellCoord p = Trinity::ComputeCellCoord(obj->GetPositionX(), obj->GetPositionY());
        if ((cell_min.x_coord <= p.x_coord && p.x_coord <= cell_max.x_coord) &&
            (cell_min.y_coord <= p.y_coord && p.y_coord <= cell_max.y_coord))
            return true;
    }

    return false;
}

template<class T>
void Map::AddToActive(T* obj)
{
    AddToActiveHelper(obj);
}

template <>
void Map::AddToActive(Creature* c)
{
    AddToActiveHelper(c);

    // also not allow unloading spawn grid to prevent creating creature clone at load
    if (!c->isPet() && c->GetDBTableGUIDLow())
    {
        float x, y, z;
        c->GetRespawnPosition(x, y, z);
        GridCoord p = Trinity::ComputeGridCoord(x, y);
        if (getNGrid(p.x_coord, p.y_coord))
            getNGrid(p.x_coord, p.y_coord)->incUnloadActiveLock();
        else
        {
            GridCoord p2 = Trinity::ComputeGridCoord(c->GetPositionX(), c->GetPositionY());
            TC_LOG_ERROR("maps", "Active creature (GUID: %u Entry: %u) added to grid[%u, %u] but spawn grid[%u, %u] was not loaded.",
                c->GetGUIDLow(), c->GetEntry(), p.x_coord, p.y_coord, p2.x_coord, p2.y_coord);
        }
    }
}

template<class T>
void Map::RemoveFromActive(T* obj)
{
    RemoveFromActiveHelper(obj);
}

template <>
void Map::RemoveFromActive(Creature* c)
{
    RemoveFromActiveHelper(c);

    // also allow unloading spawn grid
    if (!c->isPet() && c->GetDBTableGUIDLow())
    {
        float x, y, z;
        c->GetRespawnPosition(x, y, z);
        GridCoord p = Trinity::ComputeGridCoord(x, y);
        if (getNGrid(p.x_coord, p.y_coord))
            getNGrid(p.x_coord, p.y_coord)->decUnloadActiveLock();
        else
        {
            GridCoord p2 = Trinity::ComputeGridCoord(c->GetPositionX(), c->GetPositionY());
            TC_LOG_ERROR("maps", "Active creature (GUID: %u Entry: %u) removed from grid[%u, %u] but spawn grid[%u, %u] was not loaded.",
                c->GetGUIDLow(), c->GetEntry(), p.x_coord, p.y_coord, p2.x_coord, p2.y_coord);
        }
    }
}

template bool Map::AddToMap(Corpse*);
template bool Map::AddToMap(Creature*);
template bool Map::AddToMap(GameObject*);
template bool Map::AddToMap(DynamicObject*);
template bool Map::AddToMap(AreaTrigger*);
template bool Map::AddToMap(Conversation*);

template void Map::RemoveFromMap(Corpse*, bool);
template void Map::RemoveFromMap(Creature*, bool);
template void Map::RemoveFromMap(GameObject*, bool);
template void Map::RemoveFromMap(DynamicObject*, bool);
template void Map::RemoveFromMap(AreaTrigger*, bool);
template void Map::RemoveFromMap(Conversation*, bool);

template void Map::AddToActive(DynamicObject*);
template void Map::RemoveFromActive(DynamicObject*);

/* ******* Dungeon Instance Maps ******* */

InstanceMap::InstanceMap(uint32 id, time_t expiry, uint32 InstanceId, uint8 SpawnMode, Map* _parent)
  : Map(id, expiry, InstanceId, SpawnMode, _parent),
    m_resetAfterUnload(false), m_unloadWhenEmpty(false),
    i_data(NULL), i_script_id(0)
{
    //lets initialize visibility distance for dungeons
    InstanceMap::InitVisibilityDistance();

    // the timer is started by default, and stopped when the first player joins
    // this make sure it gets unloaded if for some reason no player joins
    m_unloadTimer = std::max(sWorld->getIntConfig(CONFIG_INSTANCE_UNLOAD_DELAY), (uint32)MIN_UNLOAD_DELAY);
}

InstanceMap::~InstanceMap()
{
    delete i_data;
    i_data = nullptr;
}

void InstanceMap::InitVisibilityDistance()
{
    //init visibility distance for instances
    m_VisibleDistance = World::GetMaxVisibleDistanceInInstances();
    m_VisibilityNotifyPeriod = World::GetVisibilityNotifyPeriodInInstances();
}

/*
    Do map specific checks to see if the player can enter
*/
bool InstanceMap::CanEnter(Player* player)
{
    if (player->GetMapRef().getTarget() == this)
    {
        TC_LOG_ERROR("maps", "InstanceMap::CanEnter - player %s(%u) already in map %d, %d, %d!", player->GetName(), player->GetGUIDLow(), GetId(), GetInstanceId(), GetSpawnMode());
        ABORT();
        return false;
    }

    // allow GM's to enter
    if (player->isGameMaster())
        return Map::CanEnter(player);

    // cannot enter if the instance is full (player cap), GMs don't count
    uint32 maxPlayers = GetMaxPlayers();
    if (GetPlayersCountExceptGMs() >= maxPlayers)
    {
        TC_LOG_INFO("maps", "MAP: Instance '%u' of map '%s' cannot have more than '%u' players. Player '%s' rejected", GetInstanceId(), GetMapName(), maxPlayers, player->GetName());
        player->SendTransferAborted(GetId(), TRANSFER_ABORT_MAX_PLAYERS);
        return false;
    }

    // cannot enter while an encounter is in progress on raids
    /*Group* group = player->GetGroup();
    if (!player->isGameMaster() && group && group->InCombatToInstance(GetInstanceId()) && player->GetMapId() != GetId())*/
    if (IsRaid() && GetInstanceScript() && GetInstanceScript()->IsEncounterInProgress())
    {
        player->SendTransferAborted(GetId(), TRANSFER_ABORT_ZONE_IN_COMBAT);
        return false;
    }

    // cannot enter if instance is in use by another party/soloer that have a
    // permanent save in the same instance id

    PlayerList const &playerList = GetPlayers();

    if (!playerList.isEmpty())
        for (PlayerList::const_iterator i = playerList.begin(); i != playerList.end(); ++i)
            if (Player* iPlayer = i->getSource())
            {
                if (iPlayer->isGameMaster()) // bypass GMs
                    continue;
                if (!player->GetGroup()) // player has not group and there is someone inside, deny entry
                {
                    player->SendTransferAborted(GetId(), TRANSFER_ABORT_MAX_PLAYERS);
                    return false;
                }
                // player inside instance has no group or his groups is different to entering player's one, deny entry
                if (!iPlayer->GetGroup() || iPlayer->GetGroup() != player->GetGroup())
                {
                    player->SendTransferAborted(GetId(), TRANSFER_ABORT_MAX_PLAYERS);
                    return false;
                }
                break;
            }

    return Map::CanEnter(player);
}

/*
    Do map specific checks and add the player to the map if successful.
*/
bool InstanceMap::AddPlayerToMap(Player* player, bool p_Switched /*= false*/)
{
    {
        TRINITY_GUARD(ACE_Thread_Mutex, Lock);

        // Dungeon only code
        if (IsDungeon())
        {
            Group* group = player->GetGroup();

            bool l_IsGarrisonTransfet = i_mapEntry && (i_mapEntry->Flags & MapFlags::MAP_FLAG_GARRISON) != 0;

            // increase current instances (hourly limit)
            if (!group || !group->isLFGGroup() || l_IsGarrisonTransfet)
                player->AddInstanceEnterTime(GetInstanceId(), time(NULL));

            // get or create an instance save for the map
            InstanceSave* mapSave = sInstanceSaveMgr->GetInstanceSave(GetInstanceId());
            if (!mapSave)
            {
                TC_LOG_INFO("maps", "InstanceMap::Add: creating instance save for map %d spawnmode %d with instance id %d", GetId(), GetSpawnMode(), GetInstanceId());
                mapSave = sInstanceSaveMgr->AddInstanceSave(GetId(), GetInstanceId(), Difficulty(GetSpawnMode()), 0, true);
            }

            // Check for existing instance binds for the difficulty.
            InstancePlayerBind* playerBind = player->GetBoundInstance(GetId(), Difficulty(GetSpawnMode()));
            if (playerBind && playerBind->perm)
            {
                // cannot enter other instances if bound permanently
                if (playerBind->save != mapSave)
                {
                    TC_LOG_ERROR("maps", "InstanceMap::Add: player %s(%d) is permanently bound to instance %d, %d, %d, %d, %d, %d but he is being put into instance %d, %d, %d, %d, %d, %d", player->GetName(), player->GetGUIDLow(), playerBind->save->GetMapId(), playerBind->save->GetInstanceId(), playerBind->save->GetDifficultyID(), playerBind->save->GetPlayerCount(), playerBind->save->GetGroupCount(), playerBind->save->CanReset(), mapSave->GetMapId(), mapSave->GetInstanceId(), mapSave->GetDifficultyID(), mapSave->GetPlayerCount(), mapSave->GetGroupCount(), mapSave->CanReset());
                    return false;
                }
            }
            else
            {
                if (group && !l_IsGarrisonTransfet)
                {
                    // solo saves should be reset when entering a group
                    InstanceGroupBind* groupBind = group->GetBoundInstance(this);
                    if (playerBind && playerBind->save != mapSave)
                    {
                        TC_LOG_ERROR("maps", "InstanceMap::Add: player %s(%d) is being put into instance %d, %d, %d, %d, %d, %d but he is in group %d and is bound to instance %d, %d, %d, %d, %d, %d!", player->GetName(), player->GetGUIDLow(), mapSave->GetMapId(), mapSave->GetInstanceId(), mapSave->GetDifficultyID(), mapSave->GetPlayerCount(), mapSave->GetGroupCount(), mapSave->CanReset(), GUID_LOPART(group->GetLeaderGUID()), playerBind->save->GetMapId(), playerBind->save->GetInstanceId(), playerBind->save->GetDifficultyID(), playerBind->save->GetPlayerCount(), playerBind->save->GetGroupCount(), playerBind->save->CanReset());
                        if (groupBind)
                            TC_LOG_ERROR("maps", "InstanceMap::Add: the group is bound to the instance %d, %d, %d, %d, %d, %d", groupBind->save->GetMapId(), groupBind->save->GetInstanceId(), groupBind->save->GetDifficultyID(), groupBind->save->GetPlayerCount(), groupBind->save->GetGroupCount(), groupBind->save->CanReset());
                        //ABORT();
                        return false;
                    }
                    // bind to the group or keep using the group save
                    if (!groupBind)
                        group->BindToInstance(mapSave, false);
                    else
                    {
                        // cannot jump to a different instance without resetting it
                        if (groupBind->save != mapSave)
                        {
                            TC_LOG_ERROR("maps", "InstanceMap::Add: player %s(%d) is being put into instance %d, %d, %d but he is in group %d which is bound to instance %d, %d, %d!", player->GetName(), player->GetGUIDLow(), mapSave->GetMapId(), mapSave->GetInstanceId(), mapSave->GetDifficultyID(), GUID_LOPART(group->GetLeaderGUID()), groupBind->save->GetMapId(), groupBind->save->GetInstanceId(), groupBind->save->GetDifficultyID());
                            if (mapSave)
                                TC_LOG_ERROR("maps", "MapSave players: %d, group count: %d", mapSave->GetPlayerCount(), mapSave->GetGroupCount());
                            else
                                TC_LOG_ERROR("maps", "MapSave NULL");
                            if (groupBind->save)
                                TC_LOG_ERROR("maps", "GroupBind save players: %d, group count: %d", groupBind->save->GetPlayerCount(), groupBind->save->GetGroupCount());
                            else
                                TC_LOG_ERROR("maps", "GroupBind save NULL");
                            return false;
                        }

                        // if the group/leader is permanently bound to the instance
                        // players also become permanently bound when they enter

						if (groupBind->perm)
						{
							uint32 TimeUntilLock = 60000; // Time until the player gets locked.
							bool lockExtended = false;
							bool warningOnly = false;

							WorldPacket data(SMSG_PENDING_RAID_LOCK, 10);

							data.WriteBit(warningOnly);  // events it throws:  1 : INSTANCE_LOCK_WARNING   0 : INSTANCE_LOCK_STOP / INSTANCE_LOCK_START.
							data.WriteBit(lockExtended); // extended.
							data.FlushBits();
							data << uint32(i_data ? i_data->GetCompletedEncounterMask() : 0);
							data << uint32(TimeUntilLock);
                            player->GetSession()->SendPacket(&data);
							player->SetPendingBind(mapSave->GetInstanceId(), TimeUntilLock);
						}

                        if (groupBind->perm)
                            player->SetPendingBind(mapSave->GetInstanceId(), 60000);
                    }
                }
                else
                {
                    // set up a solo bind or continue using it
                    if (!playerBind)
                        player->BindToInstance(mapSave, false);
                    else
                        if (playerBind->save != mapSave)
                            return false;
                        //ASSERT(playerBind->save == mapSave);
                }
            }

            if (group && group->isLFGGroup())
                if (uint32 dungeonId = sLFGMgr->GetDungeon(group->GetGUID(), true))
                    if (LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(dungeonId))
                        if (LFGDungeonEntry const* randomDungeon = sLFGDungeonStore.LookupEntry(*(sLFGMgr->GetSelectedDungeons(player->GetGUID()).begin())))
                            if (uint32(dungeon->map) == GetId() && dungeon->difficulty == uint32(GetDifficultyID()) && randomDungeon->type == uint32(TYPEID_RANDOM_DUNGEON))
                                player->CastSpell(player, LFG_SPELL_LUCK_OF_THE_DRAW, true);
        }

        // for normal instances cancel the reset schedule when the
        // first player enters (no players yet)
        SetResetSchedule(false);

        TC_LOG_INFO("maps", "MAP: Player '%s' entered instance '%u' of map '%s'", player->GetName(), GetInstanceId(), GetMapName());
        // initialize unload state
        m_unloadTimer = 0;
        m_resetAfterUnload = false;
        m_unloadWhenEmpty = false;
    }

    if (i_data)
        i_data->BeforePlayerEnter(player);

    // this will acquire the same mutex so it cannot be in the previous block
    Map::AddPlayerToMap(player, p_Switched);

    if (i_data)
    {
        i_data->OnPlayerEnter(player);
        i_data->UpdateCreatureGroupSizeStats();
    }

    SendInstanceGroupSizeChanged();

    /// Set raid difficulty worldstate for following world state expressions
    /// 10010, 10011, 12298, 12360, 4177, 4314, 4352, 5362, 5431, 5432, 5911, 5912, 5913, 624,
    ///  6279,  6280,  6315,  6316, 6317, 6623, 6624, 6625, 6626, 6627, 6628, 6645, 6646, 6672,
    ///  6673,  6674,  6675,  6681, 6682, 6726, 6727, 6728, 6741, 6748, 6751, 6752, 6754, 6790,
    ///  6791,  6792,  6793,  6794, 6869, 6870, 6871, 6872, 7198, 7199, 7276, 7277, 7278, 7279,
    ///  7336,  7338,  7785,  7896, 8358, 8416, 8417, 8532, 8533, 8552, 8659, 8681, 8865, 8866,
    ///  9335,  9924,  9925
    switch (GetDifficultyID())
    {
        case Difficulty::Difficulty10N:
            player->SetWorldState(2193, 0);
            break;

        case Difficulty::Difficulty25N:
            player->SetWorldState(2193, 1);
            break;

        case Difficulty::Difficulty10HC:
            player->SetWorldState(2193, 2);
            break;

        case Difficulty::Difficulty25HC:
            player->SetWorldState(2193, 3);
            break;

        default:
            player->SetWorldState(2193, 4);
            break;
    }

    return true;
}

void InstanceMap::Update(const uint32 t_diff)
{
    Map::Update(t_diff);

    if (i_data)
        i_data->Update(t_diff);
}

void InstanceMap::RemovePlayerFromMap(Player* p_Player, bool p_Remove)
{
    /// unset raid difficulty worldstate for following world state expressions
    /// 10010, 10011, 12298, 12360, 4177, 4314, 4352, 5362, 5431, 5432, 5911, 5912, 5913, 624,
    ///  6279,  6280,  6315,  6316, 6317, 6623, 6624, 6625, 6626, 6627, 6628, 6645, 6646, 6672,
    ///  6673,  6674,  6675,  6681, 6682, 6726, 6727, 6728, 6741, 6748, 6751, 6752, 6754, 6790,
    ///  6791,  6792,  6793,  6794, 6869, 6870, 6871, 6872, 7198, 7199, 7276, 7277, 7278, 7279,
    ///  7336,  7338,  7785,  7896, 8358, 8416, 8417, 8532, 8533, 8552, 8659, 8681, 8865, 8866,
    ///  9335,  9924,  9925
    p_Player->SetWorldState(2193, 4);

    /// If last player set unload timer
    if (!m_unloadTimer && m_mapRefManager.getSize() == 1)
        m_unloadTimer = m_unloadWhenEmpty ? MIN_UNLOAD_DELAY : std::max(sWorld->getIntConfig(CONFIG_INSTANCE_UNLOAD_DELAY), (uint32)MIN_UNLOAD_DELAY);

    Map::RemovePlayerFromMap(p_Player, p_Remove);

    /// For normal instances schedule the reset after all players have left
    SetResetSchedule(true);

    if (i_data)
    {
        i_data->UpdateCreatureGroupSizeStats();

        if (!p_Remove)
            i_data->OnPlayerExit(p_Player);
    }

    SendInstanceGroupSizeChanged();
}

void InstanceMap::CreateInstanceData(bool load)
{
    if (i_data != NULL)
        return;

    InstanceTemplate const* mInstance = sObjectMgr->GetInstanceTemplate(GetId());
    if (mInstance)
    {
        i_script_id = mInstance->ScriptId;
        i_data = sScriptMgr->CreateInstanceData(this);
    }

    if (!i_data)
        return;

    i_data->Initialize();

    if (load)
    {
        // TODO: make a global storage for this
        PreparedStatement* l_Statement = CharacterDatabase.GetPreparedStatement(CHAR_SEL_INSTANCE);
        l_Statement->setUInt16(0, uint16(GetId()));
        l_Statement->setUInt32(1, i_InstanceId);

        uint32 l_MapId            = GetId();
        uint32 l_InstanceId       = i_InstanceId;

        AsyncQuery(CharacterDatabase, l_Statement, [l_MapId, l_InstanceId](PreparedQueryResult const& p_Result)
        {
            Map* l_Map = sMapMgr->FindMap(l_MapId, l_InstanceId);
            if (l_Map == nullptr || l_Map->ToInstanceMap() == nullptr)
                return;

            InstanceScript* l_InstanceScript = l_Map->ToInstanceMap()->GetInstanceScript();
            if (l_InstanceScript == nullptr)
                return;

            if (p_Result)
            {
                Field* fields = p_Result->Fetch();
                std::string data = fields[0].GetString();
                l_InstanceScript->SetCompletedEncountersMask(fields[1].GetUInt32());
                if (data != "")
                    l_InstanceScript->Load(data.c_str());
            }
        });
    }
}

/*
    Returns true if there are no players in the instance
*/
bool InstanceMap::Reset(uint8 method)
{
    // note: since the map may not be loaded when the instance needs to be reset
    // the instance must be deleted from the DB by InstanceSaveManager

    if (HavePlayers())
    {
        if (method == INSTANCE_RESET_ALL || method == INSTANCE_RESET_CHANGE_DIFFICULTY)
        {
            // notify the players to leave the instance so it can be reset
            for (MapRefManager::iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
                itr->getSource()->SendResetFailedNotify();
        }
        else
        {
            if (method == INSTANCE_RESET_GLOBAL)
                // set the homebind timer for players inside (1 minute)
                for (MapRefManager::iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
                    itr->getSource()->m_InstanceValid = false;

            // the unload timer is not started
            // instead the map will unload immediately after the players have left
            m_unloadWhenEmpty = true;
            m_resetAfterUnload = true;
        }
    }
    else
    {
        // unloaded at next update
        m_unloadTimer = MIN_UNLOAD_DELAY;
        m_resetAfterUnload = true;
    }

    return m_mapRefManager.isEmpty();
}

void InstanceMap::PermBindAllPlayers(Player* p_Source)
{
    if (!IsRaidOrHeroicDungeon())
        return;

    /// Players now don't have an ID in LFR mode
    /// They can kill bosses any times they want
    /// But they can loot them only once per week
    if (IsLFR())
        return;

    InstanceSave* l_Save = sInstanceSaveMgr->GetInstanceSave(GetInstanceId());
    if (!l_Save)
    {
        TC_LOG_ERROR("maps", "Cannot bind player (GUID: %u, Name: %s), because no instance save is available for instance map (Name: %s, Entry: %u, InstanceId: %u)!",
            p_Source->GetGUIDLow(), p_Source->GetName(), p_Source->GetMap()->GetMapName(), p_Source->GetMapId(), GetInstanceId());
        return;
    }

    Group* l_Group = p_Source->GetGroup();

    // group members outside the instance group don't get bound
    for (MapRefManager::iterator l_Itr = m_mapRefManager.begin(); l_Itr != m_mapRefManager.end(); ++l_Itr)
    {
        Player* l_Player = l_Itr->getSource();
        // players inside an instance cannot be bound to other instances
        // some players may already be permanently bound, in this case nothing happens
        InstancePlayerBind* l_Bind = l_Player->GetBoundInstance(l_Save->GetMapId(), l_Save->GetDifficultyID());
        if (!l_Bind || !l_Bind->perm)
        {
            l_Player->BindToInstance(l_Save, true);

            WorldPacket l_Data(SMSG_INSTANCE_SAVE_CREATED, 4);
            l_Data.WriteBit(false);
            l_Player->GetSession()->SendPacket(&l_Data);
#ifndef CROSS
            l_Player->GetSession()->SendCalendarRaidLockout(l_Save, true);
#endif
        }

        // if the leader is not in the instance the group will not get a perm bind
        if (l_Group && l_Group->GetLeaderGUID() == l_Player->GetGUID())
            l_Group->BindToInstance(l_Save, true);
    }
}

void InstanceMap::UnloadAll()
{
    ASSERT(!HavePlayers());

    if (m_resetAfterUnload == true)
        DeleteRespawnTimes();

    Map::UnloadAll();
}

void InstanceMap::SendResetWarnings(uint32 timeLeft) const
{
    for (MapRefManager::const_iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
        itr->getSource()->SendRaidInstanceMessage(GetId(), itr->getSource()->GetDifficultyID(GetEntry()), timeLeft);
}

void InstanceMap::SendInstanceGroupSizeChanged() const
{
    for (MapRefManager::const_iterator l_Iter = m_mapRefManager.begin(); l_Iter != m_mapRefManager.end(); ++l_Iter)
        l_Iter->getSource()->SendInstanceGroupSizeChanged(m_mapRefManager.getSize());
}

void InstanceMap::SetResetSchedule(bool on)
{
    bool l_IsGarrisonMap = false;
    for (uint32 l_I = 0; l_I < sGarrSiteLevelStore.GetNumRows(); ++l_I)
    {
        const GarrSiteLevelEntry * l_Entry = sGarrSiteLevelStore.LookupEntry(l_I);

        if (l_Entry && l_Entry->MapID == GetId())
        {
            l_IsGarrisonMap = true;
            break;
        }
    }

    // only for normal instances
    // the reset time is only scheduled when there are no payers inside
    // it is assumed that the reset time will rarely (if ever) change while the reset is scheduled
    if (IsDungeon() && !HavePlayers() && !IsRaidOrHeroicDungeon() && !l_IsGarrisonMap)
    {
        if (InstanceSave* save = sInstanceSaveMgr->GetInstanceSave(GetInstanceId()))
            sInstanceSaveMgr->ScheduleReset(on, save->GetResetTime(), InstanceSaveManager::InstResetEvent(0, GetId(), Difficulty(GetSpawnMode()), GetInstanceId()));
        else
            TC_LOG_ERROR("maps", "InstanceMap::SetResetSchedule: cannot turn schedule %s, there is no save information for instance (map [id: %u, name: %s], instance id: %u, difficulty: %u)",
                on ? "on" : "off", GetId(), GetMapName(), GetInstanceId(), Difficulty(GetSpawnMode()));
    }
}

MapDifficulty const* Map::GetMapDifficulty() const
{
    return GetMapDifficultyData(GetId(), GetDifficultyID());
}


// Instance lock type for the map.
InstanceLockTypes Map::GetInstanceLockType()
{
	// Siege of Orgrimmar, Flex mode, all LFR raids and Wod raids.
	if (IsRaid() && (GetDifficultyID() == DifficultyRaidNormal || GetDifficultyID() == DifficultyRaidHeroic || GetDifficultyID() == DifficultyRaidLFR || GetDifficultyID() == DifficultyRaidTool))
		return INSTANCE_LOCK_LOOT_BASED;

	// Dungeons, Vanilla + TBC raids, WOTLK + MOP Heroic raids.
	if (IsNonRaidDungeon() || IsRaid() && (Expansion() < EXPANSION_WRATH_OF_THE_LICH_KING || Expansion() >= EXPANSION_WRATH_OF_THE_LICH_KING && (GetDifficultyID() == Difficulty10HC || GetDifficultyID() == Difficulty25HC)))
		return INSTANCE_LOCK_STRICT;

	// Normal WOTLK, MOP raids.
	if (IsRaid() && (Expansion() >= EXPANSION_WRATH_OF_THE_LICH_KING && (GetDifficultyID() == Difficulty10N || GetDifficultyID() == Difficulty25N)))
		return INSTANCE_LOCK_FLEXIBLE;

	// Not a raid / dungeon map.
	return INSTANCE_LOCK_NONE;
}

bool Map::IsHeroic() const
{
    if (DifficultyEntry const* difficulty = sDifficultyStore.LookupEntry(i_spawnMode))
        return difficulty->Flags & (DIFFICULTY_FLAG_HEROIC | DIFFICULTY_FLAG_DISPLAY_HEROIC);

    return false;
}

uint32 InstanceMap::GetMaxPlayers() const
{
    if (GetDifficultyID() == Difficulty::DifficultyRaidLFR)
        return 25;

    if (MapDifficulty const* mapDiff = GetMapDifficulty())
    {
        if (mapDiff->MaxPlayers || GetDifficultyID() == DifficultyNormal)    // Normal case (expect that regular difficulty always have correct maxplayers)
            return mapDiff->MaxPlayers;
        else                                                // DBC have 0 maxplayers for heroic instances with expansion < 2
        {                                                   // The heroic entry exists, so we don't have to check anything, simply return normal max players
            MapDifficulty const* normalDiff = GetMapDifficultyData(GetId(), DifficultyNormal);
            return normalDiff ? normalDiff->MaxPlayers : 0;
        }
    }
    else                                                    // I'd rather ABORT();
    {
        switch (GetDifficultyID())
        {
            case Difficulty::DifficultyNScenario:
            case Difficulty::DifficultyHCScenario:
                return 3;
            case Difficulty::DifficultyNormal:
            case Difficulty::DifficultyHeroic:
            case Difficulty::DifficultyChallenge:
                return 5;
            case Difficulty::Difficulty10N:
            case Difficulty::Difficulty10HC:
                return 10;
            case Difficulty::Difficulty25N:
            case Difficulty::Difficulty25HC:
            case Difficulty::DifficultyRaidLFR:
            case Difficulty::DifficultyRaidTool:
                return 25;
            case Difficulty::DifficultyRaidNormal:
            case Difficulty::DifficultyRaidHeroic:
                return 30;
            case Difficulty::DifficultyRaidMythic:
                return 20;
            default:
                break;
        }

        return 0;
    }
}

uint32 InstanceMap::GetMaxResetDelay() const
{
    MapDifficulty const* mapDiff = GetMapDifficulty();
    return mapDiff ? mapDiff->ResetTime : 0;
}

/* ******* Battleground Instance Maps ******* */

BattlegroundMap::BattlegroundMap(uint32 id, time_t expiry, uint32 InstanceId, Map* _parent, uint8 spawnMode)
  : Map(id, expiry, InstanceId, spawnMode, _parent), m_bg(NULL) { }

BattlegroundMap::~BattlegroundMap()
{
    if (m_bg)
    {
        //unlink to prevent crash, always unlink all pointer reference before destruction
        m_bg->SetBgMap(NULL);
        m_bg = NULL;
    }
}

void BattlegroundMap::InitVisibilityDistance()
{
    //init visibility distance for BG/Arenas
    m_VisibleDistance = GetBG()->isArena() ? World::GetMaxVisibleDistanceInArenas() : World::GetMaxVisibleDistanceInBG();
    m_VisibilityNotifyPeriod = World::GetVisibilityNotifyPeriodInBGArenas();
}

bool BattlegroundMap::CanEnter(Player* player)
{
    if (player->GetMapRef().getTarget() == this)
    {
        TC_LOG_ERROR("maps", "BGMap::CanEnter - player %u is already in map!", player->GetGUIDLow());
        ABORT();
        return false;
    }

    if (player->GetBattlegroundId() != GetInstanceId())
        return false;

    // player number limit is checked in bgmgr, no need to do it here

    return Map::CanEnter(player);
}

bool BattlegroundMap::AddPlayerToMap(Player* player, bool /*p_Switched*/ /*= false*/)
{
    {
        TRINITY_GUARD(ACE_Thread_Mutex, Lock);
        //Check moved to void WorldSession::HandleMoveWorldportAckOpcode()
        //if (!CanEnter(player))
            //return false;
        // reset instance validity, battleground maps do not homebind
        player->m_InstanceValid = true;
    }
    return Map::AddPlayerToMap(player);
}

void BattlegroundMap::RemovePlayerFromMap(Player* player, bool remove)
{
    TC_LOG_INFO("maps", "MAP: Removing player '%s' from bg '%u' of map '%s' before relocating to another map", player->GetName(), GetInstanceId(), GetMapName());
    Map::RemovePlayerFromMap(player, remove);
}

void BattlegroundMap::SetUnload()
{
    m_unloadTimer = MIN_UNLOAD_DELAY;
}

void BattlegroundMap::RemoveAllPlayers()
{
    if (HavePlayers())
        for (MapRefManager::iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
            if (Player* player = itr->getSource())
                if (!player->IsBeingTeleportedFar())
#ifndef CROSS
                    player->TeleportTo(player->GetBattlegroundEntryPoint());
#else /* CROSS */
                    if (InterRealmClient *irClient = InterRealmClient::GetIRClient(player))
                        irClient->RemovePlayerFromIR(player);
#endif /* CROSS */
}

Creature* Map::GetCreature(uint64 guid)
{
    return ObjectAccessor::GetObjectInMap(guid, this, (Creature*)NULL);
}

GameObject* Map::GetGameObject(uint64 guid)
{
    return ObjectAccessor::GetObjectInMap(guid, this, (GameObject*)NULL);
}

DynamicObject* Map::GetDynamicObject(uint64 guid)
{
    return ObjectAccessor::GetObjectInMap(guid, this, (DynamicObject*)NULL);
}

Transport* Map::GetTransport(uint64 guid)
{
    if (GUID_HIPART(guid) != HIGHGUID_MO_TRANSPORT)
        return NULL;

    GameObject* go = GetGameObject(guid);
    return go ? go->ToTransport() : NULL;
}

AreaTrigger* Map::GetAreaTrigger(uint64 p_Guid)
{
    return ObjectAccessor::GetObjectInMap(p_Guid, this, (AreaTrigger*)nullptr);
}

Conversation* Map::GetConversation(uint64 p_Guid)
{
    return ObjectAccessor::GetObjectInMap(p_Guid, this, (Conversation*)nullptr);
}

void Map::UpdateIteratorBack(Player* player)
{
    if (m_mapRefIter == player->GetMapRef())
        m_mapRefIter = m_mapRefIter->nocheck_prev();
}

void Map::SaveCreatureRespawnTime(uint32 dbGuid, time_t respawnTime)
{
    if (!respawnTime)
    {
        // Delete only
        RemoveCreatureRespawnTime(dbGuid);
        return;
    }

    _creatureRespawnTimes[dbGuid] = respawnTime;

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_CREATURE_RESPAWN);
    stmt->setUInt32(0, dbGuid);
    stmt->setUInt32(1, uint32(respawnTime));
    stmt->setUInt16(2, GetId());
    stmt->setUInt32(3, GetInstanceId());
    CharacterDatabase.Execute(stmt);
}

void Map::RemoveCreatureRespawnTime(uint32 dbGuid)
{
    _creatureRespawnTimes.erase(dbGuid);

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CREATURE_RESPAWN);
    stmt->setUInt32(0, dbGuid);
    stmt->setUInt16(1, GetId());
    stmt->setUInt32(2, GetInstanceId());
    CharacterDatabase.Execute(stmt);
}

void Map::SaveGORespawnTime(uint32 dbGuid, time_t respawnTime)
{
    if (!respawnTime)
    {
        // Delete only
        RemoveGORespawnTime(dbGuid);
        return;
    }

    _goRespawnTimes[dbGuid] = respawnTime;

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_GO_RESPAWN);
    stmt->setUInt32(0, dbGuid);
    stmt->setUInt32(1, uint32(respawnTime));
    stmt->setUInt16(2, GetId());
    stmt->setUInt32(3, GetInstanceId());
    CharacterDatabase.Execute(stmt);
}

void Map::RemoveGORespawnTime(uint32 dbGuid)
{
    _goRespawnTimes.erase(dbGuid);

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GO_RESPAWN);
    stmt->setUInt32(0, dbGuid);
    stmt->setUInt16(1, GetId());
    stmt->setUInt32(2, GetInstanceId());
    CharacterDatabase.Execute(stmt);
}

void Map::LoadRespawnTimes()
{
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CREATURE_RESPAWNS);
    stmt->setUInt16(0, GetId());
    stmt->setUInt32(1, GetInstanceId());
    if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
    {
        do
        {
            Field* fields      = result->Fetch();
            uint32 loguid      = fields[0].GetUInt32();
            uint32 respawnTime = fields[1].GetUInt32();

            _creatureRespawnTimes[loguid] = time_t(respawnTime);
        }
        while (result->NextRow());
    }

    stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_GO_RESPAWNS);
    stmt->setUInt16(0, GetId());
    stmt->setUInt32(1, GetInstanceId());
    if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
    {
        do
        {
            Field* fields      = result->Fetch();
            uint32 loguid      = fields[0].GetUInt32();
            uint32 respawnTime = fields[1].GetUInt32();

            _goRespawnTimes[loguid] = time_t(respawnTime);
        }
        while (result->NextRow());
    }
}

void Map::DeleteRespawnTimes()
{
    _creatureRespawnTimes.clear();
    _goRespawnTimes.clear();

    DeleteRespawnTimesInDB(GetId(), GetInstanceId());
}

void Map::DeleteRespawnTimesInDB(uint16 mapId, uint32 instanceId)
{
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CREATURE_RESPAWN_BY_INSTANCE);
    stmt->setUInt16(0, mapId);
    stmt->setUInt32(1, instanceId);
    CharacterDatabase.Execute(stmt);

    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GO_RESPAWN_BY_INSTANCE);
    stmt->setUInt16(0, mapId);
    stmt->setUInt32(1, instanceId);
    CharacterDatabase.Execute(stmt);
}

time_t Map::GetLinkedRespawnTime(uint64 guid) const
{
    uint64 linkedGuid = sObjectMgr->GetLinkedRespawnGuid(guid);
    switch (GUID_HIPART(linkedGuid))
    {
        case HIGHGUID_UNIT:
            return GetCreatureRespawnTime(GUID_LOPART(linkedGuid));
        case HIGHGUID_GAMEOBJECT:
            return GetGORespawnTime(GUID_LOPART(linkedGuid));
        default:
            break;
    }

    return time_t(0);
}

void Map::LoadAllGrids(float p_MinX, float p_MaxX, float p_MinY, float p_MaxY, Player* p_Player)
{
    float l_Step = 5.0f;
    float l_CurrX = p_MinX;
    float l_CurrY = p_MinY;

    /// Process X axis
    do
    {
        /// Process Y axis
        do
        {
            CellCoord l_CellCoord = Trinity::ComputeCellCoord(l_CurrX, l_CurrY);
            Cell l_Cell(l_CellCoord);
            EnsureGridLoadedForActiveObject(l_Cell, p_Player);
            l_CurrY += l_Step;
        }
        while (l_CurrY < p_MaxY);

        l_CurrY = p_MinY;
        l_CurrX += l_Step;
    }
    while (l_CurrX < p_MaxX);
}

CriteriaLegacyRaidType Map::GetLegacyRaidType() const
{
    switch (GetDifficultyID())
    {
        case Difficulty::Difficulty10N:
            return CriteriaLegacyRaidType::Normal10;
        case Difficulty::Difficulty25N:
            return CriteriaLegacyRaidType::Normal25;
        case Difficulty::Difficulty10HC:
            return CriteriaLegacyRaidType::Normal10;
        case Difficulty::Difficulty25HC:
            return CriteriaLegacyRaidType::Heroic25;
        default:
            return CriteriaLegacyRaidType::None;
    }
}

ItemContext Map::GetLootItemContext() const
{
    auto l_MapDifficulty = GetMapDifficulty();
    if (l_MapDifficulty && l_MapDifficulty->Context)
        return ItemContext(l_MapDifficulty->Context);

    switch (GetDifficultyID())
    {
        case Difficulty::DifficultyNormal:
            return ItemContext::DungeonNormal;
        case Difficulty::DifficultyHeroic:
            return ItemContext::DungeonHeroic;
        case Difficulty::DifficultyRaidNormal:
            return ItemContext::RaidNormal;
        case Difficulty::DifficultyRaidHeroic:
            return ItemContext::RaidHeroic;
        case Difficulty::DifficultyRaidLFR:
            return ItemContext::RaidLfr;
        case Difficulty::DifficultyRaidMythic:
            return ItemContext::RaidMythic;
        default:
            return ItemContext::None;
    }
}

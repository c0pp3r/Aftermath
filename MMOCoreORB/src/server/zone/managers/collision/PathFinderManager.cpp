/*
 * PathFinderManager.cpp
 *
 *  Created on: 02/03/2011
 *      Author: victor
 */

#include "PathFinderManager.h"
#include "server/zone/objects/building/BuildingObject.h"
#include "server/zone/objects/cell/CellObject.h"
#include "templates/SharedObjectTemplate.h"
#include "templates/appearance/PortalLayout.h"
#include "templates/appearance/FloorMesh.h"
#include "templates/appearance/PathGraph.h"
#include "server/zone/Zone.h"

#include "CollisionManager.h"
#include "engine/util/u3d/Funnel.h"
#include "engine/util/u3d/Segment.h"
#include "pathfinding/recast/DetourCommon.h"

void destroyNavMeshQuery(void* value) {
	dtFreeNavMeshQuery(reinterpret_cast<dtNavMeshQuery*>(value));
}

PathFinderManager::PathFinderManager() : Logger("PathFinderManager"), m_navQuery(destroyNavMeshQuery) {
	setFileLogger("log/pathfinder.log");

	m_filter.setIncludeFlags(SAMPLE_POLYFLAGS_ALL ^ SAMPLE_POLYFLAGS_DISABLED);
	m_filter.setExcludeFlags(0);
	m_filter.setAreaCost(SAMPLE_POLYAREA_GROUND, 1.0f);
	m_filter.setAreaCost(SAMPLE_POLYAREA_WATER, 15.0f);
	m_filter.setAreaCost(SAMPLE_POLYAREA_ROAD, 1.0f);
	m_filter.setAreaCost(SAMPLE_POLYAREA_DOOR, 1.0f);
	m_filter.setAreaCost(SAMPLE_POLYAREA_GRASS, 2.0f);
	m_filter.setAreaCost(SAMPLE_POLYAREA_JUMP, 1.5f);

	m_spawnFilter.setIncludeFlags(SAMPLE_POLYFLAGS_ALL ^ (SAMPLE_POLYFLAGS_DISABLED | SAMPLE_POLYFLAGS_SWIM));
	m_spawnFilter.setAreaCost(SAMPLE_POLYAREA_GROUND, 1.0f);
	m_spawnFilter.setExcludeFlags(0);

	setLogging(true);
}

Vector<WorldCoordinates>* PathFinderManager::findPath(const WorldCoordinates& pointA, const WorldCoordinates& pointB, Zone *zone) {
	if (std::isnan(pointA.getX()) || std::isnan(pointA.getY()) || std::isnan(pointA.getZ()))
		return NULL;

	if (std::isnan(pointB.getX()) || std::isnan(pointB.getY()) || std::isnan(pointB.getZ()))
		return NULL;

	CellObject* cellA = pointA.getCell();
	CellObject* cellB = pointB.getCell();

	if (cellA == NULL && cellB == NULL) { // world -> world
		return findPathFromWorldToWorld(pointA, pointB, zone);
	} else if (cellA != NULL && cellB == NULL) { // cell -> world
		return findPathFromCellToWorld(pointA, pointB, zone);
	} else if (cellA == NULL && cellB != NULL) { // world -> cell
		return findPathFromWorldToCell(pointA, pointB, zone);
	} else /* if (cellA != NULL && cellB != NULL) */ { // cell -> cell, the only left option
		return findPathFromCellToCell(pointA, pointB);
	}
}

void PathFinderManager::filterPastPoints(Vector<WorldCoordinates>* path, SceneObject* object) {
    Vector3 thisWorldPosition = object->getWorldPosition();
    Vector3 thiswP = thisWorldPosition;
    thiswP.setZ(0);

    if (path->size() > 2 && path->get(0) == path->get(1))
        path->remove(1);

    for (int i = 2; i < path->size(); ++i) {
        WorldCoordinates coord1 = path->get(i);
        WorldCoordinates coord2 = path->get(i - 1);

        Vector3 end = coord1.getWorldPosition();
        Vector3 start = coord2.getWorldPosition();

        if (coord1.getCell() != coord2.getCell()) {
            Vector3 coord1WorldPosition = end;
            Vector3 coord2WorldPosition = start;

            if (coord1WorldPosition == coord2WorldPosition && thisWorldPosition == coord1WorldPosition) {
                path->remove(i - 1);
                break;
            }

            continue;
        }

        end.setZ(0);
        start.setZ(0);
        Segment sgm(start, end);

        Vector3 closestP = sgm.getClosestPointTo(thiswP);

        if (closestP.distanceTo(thiswP) <= FLT_EPSILON) {
            for (int j = i - 1; j > 0; --j) {
                path->remove(j);
            }

            break;
        }
    }
}

bool pointInSphere(const Vector3 &point, const Sphere& sphere) {
	return (point-sphere.getCenter()).length() < sphere.getRadius();
}

static AtomicLong totalTime;

void PathFinderManager::getNavMeshCollisions(SortedVector<NavCollision*> *collisions,
											 const SortedVector<ManagedReference<NavMeshRegion*>> *regions,
											 const Vector3& start, const Vector3& end) {
	Vector3 dir = (end-start);
	float maxT = dir.normalize();

	for (const ManagedReference<NavMeshRegion*>& region : *regions) {
		const AABB* bounds = region->getMeshBounds();

		const Vector3& bPos = bounds->center();
		Vector3 sPos(bPos.getX(), bPos.getZ(), 0);
		const float radius = bounds->extents()[bounds->longestAxis()] * .975f;
		float radiusSq = radius*radius;

		//http://www.scratchapixel.com/code.php?id=10&origin=/lessons/3d-basic-rendering/minimal-ray-tracer-rendering-simple-shapes
		Vector3 L = sPos - start;
		float tca = L.dotProduct(dir);
		if (tca < 0) continue;
		float d2 = L.dotProduct(L) - tca * tca;
		if (d2 > radiusSq) continue;
		float thc = sqrt(radiusSq - d2);
		float t1 = tca - thc;
		float t2 = tca + thc;

		if (abs(t1 - t2) > 0.1f && t1 > 0 && t1 < maxT)
			collisions->put(new NavCollision(start + (dir * t1), t1, region));

		if (t2 > 0 && t2 < maxT)
			collisions->put(new NavCollision(start + (dir * t2), t2, region));
	}
}

bool PathFinderManager::getRecastPath(const Vector3& start, const Vector3& end, NavMeshRegion* region, Vector<WorldCoordinates>* path, float& len, bool allowPartial) {
	const Vector3 startPosition(start.getX(), start.getZ(), -start.getY());
	const Vector3 targetPosition(end.getX(), end.getZ(), -end.getY());
	const float* startPosAsFloat = startPosition.toFloatArray();
	const float* tarPosAsFloat = targetPosition.toFloatArray();
	static float extents[3] = {2, 4, 2};
	dtPolyRef startPoly;
	dtPolyRef endPoly;

	Reference<RecastNavMesh*> navMesh = region->getNavMesh();

	if(navMesh == NULL || navMesh->isLoaded() == false)
		return 0;

	dtNavMeshQuery *query = m_navQuery.get();
	if(query == NULL) {
		query = dtAllocNavMeshQuery();
		m_navQuery.set(query);
	}

	const Vector3& regionPos = region->getPosition();
	// We need to flip the Y/Z axis and negate Z to put it in recasts model space
	const Sphere sphere(Vector3(regionPos.getX(), regionPos.getZ(), -regionPos.getY()), region->getRadius());

	query->init(navMesh->getNavMesh(), 2048);

	if (pointInSphere(targetPosition, sphere) || pointInSphere(startPosition, sphere)) {

		Vector3 polyStart;
		Vector3 polyEnd;
		int numPolys;
		dtPolyRef polyPath[2048];
		int status = 0;

		ReadLocker rLocker(navMesh->getLock());

		if (!((status =query->findNearestPoly(startPosAsFloat, extents, &m_filter, &startPoly, polyStart.toFloatArray())) & DT_SUCCESS))
			return false;

		if (!((status = query->findNearestPoly(tarPosAsFloat, extents, &m_filter, &endPoly, polyEnd.toFloatArray())) & DT_SUCCESS))
			return false;

		if (!((status = query->findPath(startPoly, endPoly, polyStart.toFloatArray(), polyEnd.toFloatArray(), &m_filter, polyPath, &numPolys, 2048)) & DT_SUCCESS) && !allowPartial)
			return false;

		if (numPolys) {
			// In case of partial path, make sure the end point is clamped to the last polygon.
			float epos[3];
			dtVcopy(epos, polyEnd.toFloatArray());
			if (polyPath[numPolys - 1] != endPoly) {
				
#ifdef DEBUG_PATHING
				info("Poly mismatch: Expected: " + String::hexvalueOf((int64)endPoly) + " actual: " + String::hexvalueOf((int64)polyPath[numPolys-1]), true);
#endif

				if (allowPartial)
					query->closestPointOnPoly(polyPath[numPolys - 1], tarPosAsFloat, polyEnd.toFloatArray(), 0);
				else
					return false;
			}

			unsigned char flags[128];
			float pathPoints[128][3];
			int numPoints = 0;

			status = query->findStraightPath(polyStart.toFloatArray(), polyEnd.toFloatArray(),
									polyPath, numPolys,
									(float*) pathPoints, NULL, NULL,
									&numPoints, 128, 0);

			if (numPoints > 0) {
				for (int i = 0; i < numPoints; i++) {
					//info("PathFind Point : " + point.toString(), true);
					len += pathPoints[i][0] * pathPoints[i][0] + pathPoints[i][2] * pathPoints[i][2];
					path->add(WorldCoordinates(Vector3(pathPoints[i][0], -pathPoints[i][2], pathPoints[i][1]),
											   NULL));
				}
			}
		}
	}
	return true;
}

Vector<WorldCoordinates>* PathFinderManager::findPathFromWorldToWorld(const WorldCoordinates& pointA, Vector<WorldCoordinates>& endPoints, Zone* zone, bool allowPartial) {
	Vector<WorldCoordinates>* finalpath = new Vector<WorldCoordinates>();
	float finalLengthSq = FLT_MAX;

#ifdef PROFILE_PATHING
	Timer t;
	t.start();
#endif

	for(const WorldCoordinates& pointB : endPoints) {
		const Vector3& startTemp = pointA.getPoint();
		const Vector3& targetTemp = pointB.getPoint();

		SortedVector<ManagedReference<NavMeshRegion*> > regions;

		Vector3 mid = startTemp + ((targetTemp-startTemp) * 0.5f);

		zone->getInRangeNavMeshes(mid.getX(), mid.getY(), startTemp.distanceTo(targetTemp), &regions, false);

		SortedVector<NavCollision*> collisions;
		getNavMeshCollisions(&collisions, &regions, pointA.getWorldPosition(), pointB.getWorldPosition());
		// Collisions are sorted by distance from the start of the line. This is done so that we can chain our path from
		// one navmesh to another if a path spans multiple regions.
		Vector<WorldCoordinates> *path = new Vector<WorldCoordinates>();
		float len = 0.0f;

		try {
			if (collisions.size() == 1) { // we're entering a navmesh
				NavCollision* collision = collisions.get(0);
				NavMeshRegion *region = collision->getRegion();
				Vector3 position = collision->getPosition();
				position.setZ(zone->getHeightNoCache(position.getX(), position.getY()));

				path->add(pointA);

				if (!getRecastPath(position, targetTemp, region, path, len, allowPartial)) { // entering navmesh
						continue;
				}

				if (len > 0 && len < finalLengthSq) {
					if (finalpath)
						delete finalpath;

					finalLengthSq = len;
					finalpath = path;
					path = NULL;
				}
			} else { // we're already inside a navmesh
				for (int i = 0; i < regions.size(); i++) {
					if (!getRecastPath(startTemp, targetTemp, regions.get(i), path, len, allowPartial)) {
						continue;
					}

					if (len > 0 && len < finalLengthSq) {
						if (finalpath)
							delete finalpath;

						finalLengthSq = len;
						finalpath = path;
						path = new Vector<WorldCoordinates>();
					}
				}
			}
		} catch (...) {
			error("Unhandled pathing exception");

			for (int i=collisions.size()-1; i>=0; i--) {
				NavCollision *collision = collisions.remove(i);
				delete collision;
			}

			delete path;
			path = NULL;
		}

		for (int i=collisions.size()-1; i>=0; i--) {
			NavCollision *collision = collisions.remove(i);
			delete collision;
		}

		if (path != NULL)
			delete path;
	}

	if (finalpath && finalpath->size() < 2) { // path could not be evaluated, just return the start/end position
		finalpath->removeAll();
		finalpath->add(pointA);
		finalpath->add(endPoints.get(0));
	}

#ifdef PROFILE_PATHING
	t.stop();
	totalTime.add(t.getElapsedTime());
	info("Spent " + String::valueOf(totalTime.get()) + " in recast", true);
#endif
	return finalpath;
}

Vector<WorldCoordinates>* PathFinderManager::findPathFromWorldToWorld(const WorldCoordinates& pointA, const WorldCoordinates& pointB, Zone* zone) {
	Vector<WorldCoordinates> temp;
	temp.add(pointB);
	return findPathFromWorldToWorld(pointA, temp, zone, true);
}

Vector<WorldCoordinates>* PathFinderManager::findPathFromWorldToCell(const WorldCoordinates& pointA, const WorldCoordinates& pointB, Zone *zone) {
	CellObject* targetCell = pointB.getCell();

	if (targetCell == NULL)
		return NULL;

	ManagedReference<BuildingObject*> building = dynamic_cast<BuildingObject*>(targetCell->getParent().get().get());

	if (building == NULL) {
		error("building == NULL in PathFinderManager::findPathFromWorldToCell");
		return NULL;
	}

	SharedObjectTemplate* templateObject = building->getObjectTemplate();

	if (templateObject == NULL)
		return NULL;

	PortalLayout* portalLayout = templateObject->getPortalLayout();

	if (portalLayout == NULL)
		return NULL;

	//find nearest entrance
	FloorMesh* exteriorFloorMesh = portalLayout->getFloorMesh(0); // get outside layout

	if (exteriorFloorMesh == NULL)
		return NULL;

	PathGraph* exteriorPathGraph = exteriorFloorMesh->getPathGraph();

	FloorMesh* targetFloorMesh = portalLayout->getFloorMesh(targetCell->getCellNumber());
	PathGraph* targetPathGraph = targetFloorMesh->getPathGraph();

	Vector<WorldCoordinates>* path = new Vector<WorldCoordinates>(5, 1);
	path->add(pointA);

	Vector3 transformedPosition = transformToModelSpace(pointA.getPoint(), building);

	PathNode* nearestEntranceNode = exteriorPathGraph->findNearestNode(transformedPosition);

	if (nearestEntranceNode == NULL) {
		error("NULL entrance node for building " + templateObject->getFullTemplateString());
		delete path;
		return NULL;
	}
	//PathNode* nearestTargetNode = targetPathGraph->findNearestNode(pointB.getPoint());
	TriangleNode* nearestTargetNodeTriangle = CollisionManager::getTriangle(pointB.getPoint(), targetFloorMesh);

	if (nearestTargetNodeTriangle == NULL) {
		delete path;
		return NULL;
	}

	PathNode* nearestTargetNode = CollisionManager::findNearestPathNode(nearestTargetNodeTriangle, targetFloorMesh, pointB.getPoint());//targetPathGraph->findNearestNode(pointB.getPoint());

	if (nearestTargetNode == NULL) {
		delete path;
		return NULL;
	}

	/*if (nearestEntranceNode == nearestTargetNode)
		info("nearestEntranceNode == nearestTargetNode", true);*/

	//find graph from outside to appropriate cell
	Vector<PathNode*>* pathToCell = portalLayout->getPath(nearestEntranceNode, nearestTargetNode);

	if (pathToCell == NULL) {
		error("pathToCell = portalLayout->getPath(nearestEntranceNode, nearestTargetNode); == NULL");
		delete path;
		return NULL;
	}

	for (int i = 0; i < pathToCell->size(); ++i) {
		PathNode* pathNode = pathToCell->get(i);
		PathGraph* pathGraph = pathNode->getPathGraph();

		FloorMesh* floorMesh = pathGraph->getFloorMesh();

		int cellID = floorMesh->getCellID();

		//info("cellID:" + String::valueOf(cellID), true);

		if (cellID == 0) { // we are still outside
			WorldCoordinates coord(pathNode->getPosition(), targetCell);

			path->add(WorldCoordinates(coord.getWorldPosition(), NULL));
		} else { // we are inside the building
			CellObject* pathCell = building->getCell(cellID);

			path->add(WorldCoordinates(pathNode->getPosition(), pathCell));

			if (i == pathToCell->size() - 1)
				if (pathCell != targetCell) {
					error("final cell not target cell");
				}
		}
	}

	delete pathToCell;
	pathToCell = NULL;

	// path from cell path node to destination point
	Vector<Triangle*>* trianglePath = NULL;

	int res = getFloorPath(path->get(path->size() - 1).getPoint(), pointB.getPoint(), targetFloorMesh, trianglePath);

	if (res != -1 && trianglePath != NULL)
		addTriangleNodeEdges(path->get(path->size() - 1).getPoint(), pointB.getPoint(), trianglePath, path, targetCell);

	if (trianglePath != NULL)
		delete trianglePath;

	path->add(pointB);

	return path;
}

FloorMesh* PathFinderManager::getFloorMesh(CellObject* cell) {
    ManagedReference<BuildingObject*> building1 = (cell->getParent().castTo<BuildingObject*>());

    SharedObjectTemplate* templateObject = building1->getObjectTemplate();

    if (templateObject == NULL) {
    	return NULL;
    }

    PortalLayout* portalLayout = templateObject->getPortalLayout();

    if (portalLayout == NULL) {
    	return NULL;
    }

    FloorMesh* floorMesh1 = portalLayout->getFloorMesh(cell->getCellNumber());

    return floorMesh1;
}

int PathFinderManager::getFloorPath(const Vector3& pointA, const Vector3& pointB, FloorMesh* floor, Vector<Triangle*>*& nodes) {
	/*Vector3 objectPos = pointA;
	Vector3 targetPos = pointB;

	StringBuffer objPos;
	objPos << "returning path point x:" << objectPos.getX() << " z:" << objectPos.getZ() << " y:" << objectPos.getY();
	info(objPos.toString(), true);

	//switching y<->z for client coords
	objectPos.set(objectPos.getX(), objectPos.getY(), objectPos.getZ());
	targetPos.set(targetPos.getX(), targetPos.getY(), targetPos.getZ());*/

	//TriangleNode* objectFloor = floor->findNearestTriangle(objectPos);

	TriangleNode* objectFloor = CollisionManager::getTriangle(pointA, floor);

	/*Vector3 objectFloorVector = objectFloor->getBarycenter();
	StringBuffer objectFoorBar;
	objectFoorBar << "nearest object floor point x:" << objectFloorVector.getX() << " z:" << objectFloorVector.getY() << " y:" << objectFloorVector.getZ();
	info(objectFoorBar.toString(), true);*/

	//TriangleNode* targetFloor = floor->findNearestTriangle(targetPos);
	TriangleNode* targetFloor = CollisionManager::getTriangle(pointB, floor);

	/*Vector3 targetFloorVector = targetFloor->getBarycenter();
	StringBuffer targetFoorBar;
	targetFoorBar << "nearest target floor point x:" << targetFloorVector.getX() << " z:" << targetFloorVector.getY() << " y:" << targetFloorVector.getZ();
	info(targetFoorBar.toString(), true);*/

	nodes = NULL;

	if (objectFloor == targetFloor) { // we are on the same triangle, returning pointB
		return -1;
	} else if (objectFloor == NULL || targetFloor == NULL)
		return 1;

	nodes = TriangulationAStarAlgorithm::search(pointA, pointB, objectFloor, targetFloor);

	if (nodes == NULL)
		return 1;

	return 0;
}

Vector3 PathFinderManager::transformToModelSpace(const Vector3& point, SceneObject* building) {
	// we need to move world position into model space
	Vector3 switched(point.getX(), point.getZ(), point.getY());
	Matrix4 translationMatrix;
	translationMatrix.setTranslation(-building->getPositionX(), -building->getPositionZ(), -building->getPositionY());

	float rad = -building->getDirection()->getRadians();
	float cosRad = cos(rad);
	float sinRad = sin(rad);

	Matrix3 rot;
	rot[0][0] = cosRad;
	rot[0][2] = -sinRad;
	rot[1][1] = 1;
	rot[2][0] = sinRad;
	rot[2][2] = cosRad;

	Matrix4 rotateMatrix;
	rotateMatrix.setRotationMatrix(rot);

	Matrix4 modelMatrix;
	modelMatrix = translationMatrix * rotateMatrix;

	Vector3 transformedPosition = switched * modelMatrix;

	transformedPosition.set(transformedPosition.getX(), transformedPosition.getY(), transformedPosition.getZ());

	return transformedPosition;
}

Vector<WorldCoordinates>* PathFinderManager::findPathFromCellToWorld(const WorldCoordinates& pointA, const WorldCoordinates& pointB, Zone *zone) {
	Vector<WorldCoordinates>* path = new Vector<WorldCoordinates>(5, 1);

	if (path == NULL)
		return NULL;

	path->add(pointA);

	CellObject* ourCell = pointA.getCell();
	ManagedReference<BuildingObject*> building = cast<BuildingObject*>( ourCell->getParent().get().get());
	int ourCellID = ourCell->getCellNumber();
	SharedObjectTemplate* templateObject = ourCell->getParent().get()->getObjectTemplate();

	if (templateObject == NULL) {
		delete path;
		return NULL;
	}

	PortalLayout* portalLayout = templateObject->getPortalLayout();

	if (portalLayout == NULL) {
		delete path;
		return NULL;
	}

	FloorMesh* sourceFloorMesh = portalLayout->getFloorMesh(ourCellID);

	if (sourceFloorMesh == NULL) {
		delete path;
		return NULL;
	}

	PathGraph* sourcePathGraph = sourceFloorMesh->getPathGraph();

	if (sourcePathGraph == NULL) {
		delete path;
		return NULL;
	}

	FloorMesh* exteriorFloorMesh = portalLayout->getFloorMesh(0);

	if (exteriorFloorMesh == NULL) {
		delete path;
		return NULL;
	}

	PathGraph* exteriorPathGraph = exteriorFloorMesh->getPathGraph();

	if (exteriorPathGraph == NULL) {
		delete path;
		return NULL;
	}

	// we need to move world position into model space
	Vector3 transformedPosition = transformToModelSpace(pointB.getPoint(), building);

	//find exit node in our cell
	//PathNode* exitNode = sourcePathGraph->findNearestNode(pointA.getPoint());
	TriangleNode* nearestTargetNodeTriangle = CollisionManager::getTriangle(pointA.getPoint(), sourceFloorMesh);

	if (nearestTargetNodeTriangle == NULL) {
		delete path;
		return NULL;
	}

	PathNode* exitNode = CollisionManager::findNearestPathNode(nearestTargetNodeTriangle, sourceFloorMesh, transformedPosition);//targetPathGraph->findNearestNode(pointB.getPoint());

	if (exitNode == NULL) {
		delete path;
		return NULL;
	}

	//find exterior node
	PathNode* exteriorNode = exteriorPathGraph->findNearestGlobalNode(transformedPosition);

	if (exteriorNode == NULL) {
		delete path;
		return NULL;
	}

	//find path to the exit
	Vector<PathNode*>* exitPath = portalLayout->getPath(exitNode, exteriorNode);

	if (exitPath == NULL) {
		error("exitPath == NULL");
		delete path;
		return NULL;
	}

	//find triangle path to exitNode
	Vector<Triangle*>* trianglePath = NULL;

	int res = getFloorPath(pointA.getPoint(), exitNode->getPosition(), sourceFloorMesh, trianglePath);

	if (res != -1 && trianglePath != NULL)
		addTriangleNodeEdges(pointA.getPoint(), exitNode->getPosition(), trianglePath, path, ourCell);

	if (trianglePath != NULL)
		delete trianglePath;

	path->add(WorldCoordinates(exitNode->getPosition(), ourCell));

	//populate cell traversing
	for (int i = 0; i < exitPath->size(); ++i) {
		PathNode* pathNode = exitPath->get(i);
		PathGraph* pathGraph = pathNode->getPathGraph();

		FloorMesh* floorMesh = pathGraph->getFloorMesh();

		int cellID = floorMesh->getCellID();

		if (cellID == 0) { // we are outside
			WorldCoordinates coord(pathNode->getPosition(), ourCell);

			path->add(WorldCoordinates(coord.getWorldPosition(), NULL));
		} else { // we are inside the building
			CellObject* pathCell = building->getCell(cellID);

			path->add(WorldCoordinates(pathNode->getPosition(), pathCell));
		}
	}

	delete exitPath;
	exitPath = NULL;
	
	if (path->size()) {
		Vector<WorldCoordinates>* newPath = findPathFromWorldToWorld(path->get(path->size()-1), pointB, zone);
		if (newPath) {
			path->addAll(*newPath);
			delete newPath;
		}
	} else
		path->add(pointB);

	return path;
}

void PathFinderManager::addTriangleNodeEdges(const Vector3& source, const Vector3& goal, Vector<Triangle*>* trianglePath, Vector<WorldCoordinates>* path, CellObject* cell) {
	Vector3 startPoint = Vector3(source.getX(), source.getZ(), source.getY());
	Vector3 goalPoint = Vector3(goal.getX(), goal.getZ(), goal.getY());

	Vector<Vector3>* funnelPath = Funnel::funnel(startPoint, goalPoint, trianglePath);

	/*info("found funnel size: " + String::valueOf(funnelPath->size()));
	info("triangle number: " + String::valueOf(trianglePath->size()));

	StringBuffer objectFoorBarSource;
	objectFoorBarSource << "funnel source point x:" << source.getX() << " z:" << source.getZ() << " y:" << source.getY();
	info(objectFoorBarSource.toString(), true);*/

	for (int i = 1; i < funnelPath->size() - 1; ++i) { //without the start and the goal
		/*Vector3 worldPosition = path->get(i);

		StringBuffer objectFoorBar;
		objectFoorBar << "funnel node point x:" << worldPosition.getX() << " z:" << worldPosition.getZ() << " y:" << worldPosition.getY();
		info(objectFoorBar.toString(), true);*/

		Vector3 pathPoint = funnelPath->get(i);

		//switch y<->x
		pathPoint.set(pathPoint.getX(), pathPoint.getY(), pathPoint.getZ());

		/*StringBuffer objectFoorBar;
		objectFoorBar << "funnel node point x:" << pathPoint.getX() << " z:" << pathPoint.getZ() << " y:" << pathPoint.getY();
		info(objectFoorBar.toString(), true);*/

		path->add(WorldCoordinates(pathPoint, cell));
	}

	/*StringBuffer objectFoorBarGoal;
	objectFoorBarGoal << "funnel goal point x:" << goal.getX() << " z:" << goal.getZ() << " y:" << goal.getY();
	info(objectFoorBarGoal.toString(), true);*/

	delete funnelPath;



	/*for (int i = 0; i < trianglePath->size(); ++i) {
		TriangleNode* node = trianglePath->get(i);

		//if (!node->isEdge()) // adding only edge nodes
			//continue;

		Vector3 pathBary = node->getBarycenter();

		//switch y<->x
		pathBary.set(pathBary.getX(), pathBary.getY(), pathBary.getZ());

		path->add(WorldCoordinates(pathBary, cell));
	}*/
}

Vector<WorldCoordinates>* PathFinderManager::findPathFromCellToDifferentCell(const WorldCoordinates& pointA, const WorldCoordinates& pointB) {
	//info ("findPathFromCellToDifferentCell", true);

	CellObject* ourCell = pointA.getCell();
	CellObject* targetCell = pointB.getCell();

	int ourCellID = ourCell->getCellNumber();
	int targetCellID = targetCell->getCellNumber();

	ManagedReference<BuildingObject*> building1 = cast<BuildingObject*>( ourCell->getParent().get().get());
	ManagedReference<BuildingObject*> building2 = cast<BuildingObject*>( targetCell->getParent().get().get());

	if (building1 != building2) // TODO: implement path finding between 2 buildings
		return NULL;

	SharedObjectTemplate* templateObject = building1->getObjectTemplate();

	if (templateObject == NULL)
		return NULL;

	PortalLayout* portalLayout = templateObject->getPortalLayout();

	if (portalLayout == NULL)
		return NULL;

	FloorMesh* floorMesh1 = portalLayout->getFloorMesh(ourCellID);
	FloorMesh* floorMesh2 = portalLayout->getFloorMesh(targetCellID);

	if (floorMesh2->getCellID() != targetCellID)
		error("floorMes2 cellID != targetCellID");

	//info("targetCellID:" + String::valueOf(targetCellID), true);

	PathGraph* pathGraph1 = floorMesh1->getPathGraph();
	PathGraph* pathGraph2 = floorMesh2->getPathGraph();

	Vector<WorldCoordinates>* path = new Vector<WorldCoordinates>(5, 1);
	path->add(pointA); // adding source

	//PathNode* source = pathGraph1->findNearestNode(pointA.getPoint());
	TriangleNode* nearestSourceNodeTriangle = CollisionManager::getTriangle(pointA.getPoint(), floorMesh1);

	if (nearestSourceNodeTriangle == NULL) {
		delete path;
		return NULL;
	}

	PathNode* source = CollisionManager::findNearestPathNode(nearestSourceNodeTriangle, floorMesh1, pointB.getPoint());//targetPathGraph->findNearestNode(pointB.getPoint());

	if (source == NULL) {
		delete path;
		return NULL;
	}

	//PathNode* target = pathGraph2->findNearestNode(pointB.getPoint());
	TriangleNode* nearestTargetNodeTriangle = CollisionManager::getTriangle(pointB.getPoint(), floorMesh2);

	if (nearestTargetNodeTriangle == NULL) {
		delete path;
		return NULL;
	}

	PathNode* target = CollisionManager::findNearestPathNode(nearestTargetNodeTriangle, floorMesh2, pointB.getPoint());//targetPathGraph->findNearestNode(pointB.getPoint());

	if (target == NULL) {
		delete path;
		return NULL;
	}

	Vector<PathNode*>* nodes = portalLayout->getPath(source, target);

	if (nodes == NULL) {
		StringBuffer str;
		str << "Could not find path from node: " << source->getID()
				<< " to node: " << target->getID() << " in building: "
				<< templateObject->getFullTemplateString();

		log(str.toString());

		delete path;
		return NULL;
	}

	// FIXME (dannuic): Sometimes nodes only have one entry.... why?
	if (nodes->size() == 1) {
		error("Only one node");

		delete path;
		return NULL;
	}

	// path from our position to path node
	Vector<Triangle*>* trianglePath = NULL;

	int res = getFloorPath(pointA.getPoint(), nodes->get(1)->getPosition(), floorMesh1, trianglePath);

	if (res != -1 && trianglePath != NULL)
		addTriangleNodeEdges(pointA.getPoint(), nodes->get(1)->getPosition(), trianglePath, path, ourCell);

	if (trianglePath != NULL) {
		delete trianglePath;
		trianglePath = NULL;
	}

	path->add(WorldCoordinates(source->getPosition(), ourCell));

	//traversing cells
	for (int i = 1; i < nodes->size(); ++i) {
		PathNode* pathNode = nodes->get(i);
		PathGraph* pathGraph = pathNode->getPathGraph();

		FloorMesh* floorMesh = pathGraph->getFloorMesh();

		int cellID = floorMesh->getCellID();

		if (cellID == 0) {
			//info("cellID == 0", true);
			WorldCoordinates coord(pathNode->getPosition(), ourCell);

			path->add(WorldCoordinates(coord.getWorldPosition(), NULL));
		} else {
			CellObject* pathCell = building1->getCell(cellID);

			WorldCoordinates coord(pathNode->getPosition(), pathCell);

			path->add(coord);

			//info("cellID:" + String::valueOf(cellID), true);

			if (i == nodes->size() - 1) {
				if (pathNode != target) {
					StringBuffer msg;
					msg << "pathNode != target pathNode: " << pathNode->getID() << " target:" << target->getID();
					error(msg.toString());
				}

				if (pathCell != targetCell) {
					error("final cell not target cell");
				}
			}
		}
	}

	delete nodes;
	nodes = NULL;

	// path from cell entrance to destination point
	trianglePath = NULL;

	res = getFloorPath(path->get(path->size() - 1).getPoint(), pointB.getPoint(), floorMesh2, trianglePath);

	if (res != -1 && trianglePath != NULL)
		addTriangleNodeEdges(path->get(path->size() - 1).getPoint(), pointB.getPoint(), trianglePath, path, targetCell);

	if (trianglePath != NULL)
		delete trianglePath;

	path->add(pointB);

	return path;
}

Vector<WorldCoordinates>* PathFinderManager::findPathFromCellToCell(const WorldCoordinates& pointA, const WorldCoordinates& pointB) {
	CellObject* ourCell = pointA.getCell();
	CellObject* targetCell = pointB.getCell();

	if (ourCell != targetCell)
		return findPathFromCellToDifferentCell(pointA, pointB);

	int ourCellID = ourCell->getCellNumber();

	ManagedReference<BuildingObject*> building = cast<BuildingObject*>( ourCell->getParent().get().get());

	SharedObjectTemplate* templateObject = building->getObjectTemplate();

	if (templateObject == NULL)
		return NULL;

	PortalLayout* portalLayout = templateObject->getPortalLayout();

	if (portalLayout == NULL)
		return NULL;

	FloorMesh* floorMesh1 = portalLayout->getFloorMesh(ourCellID);
	PathGraph* pathGraph1 = floorMesh1->getPathGraph();

	Vector<WorldCoordinates>* path = new Vector<WorldCoordinates>(5, 1);
	path->add(pointA); // adding source

	//info("same cell... trying to calculate triangle path", true);

	Vector<Triangle*>* trianglePath = NULL;

	//info("searching floorMesh for cellID " + String::valueOf(ourCellID), true);

	int res = getFloorPath(pointA.getPoint(), pointB.getPoint(), floorMesh1, trianglePath);

	if (res == -1) { //points in the same triangle
		path->add(pointB);

		return path;
	}

	if (trianglePath == NULL) { // returning NULL, no path found
		//error("path NULL");
		delete path;

		return findPathFromCellToDifferentCell(pointA, pointB);
	} else {
		//info("path found", true);

		addTriangleNodeEdges(pointA.getPoint(), pointB.getPoint(), trianglePath, path, ourCell);

		delete trianglePath;

		path->add(pointB); //adding destination

		return path;
	}

	return path;
}

float frand() {
	return System::getMTRand()->randExc();
}


bool PathFinderManager::getSpawnPointInArea(const Sphere& area, Zone *zone, Vector3& point) {
	SortedVector<ManagedReference<NavMeshRegion*>> regions;
	float radius = area.getRadius();
	const Vector3& center = area.getCenter();
	Vector3 flipped(center.getX(), center.getZ(), -center.getY());
	float extents[3] = {3, 5, 3};

	dtNavMeshQuery *query = m_navQuery.get();
	if(query == NULL) {
		query = dtAllocNavMeshQuery();
		m_navQuery.set(query);
	}

	if (zone == NULL)
		return false;

	zone->getInRangeNavMeshes(center.getX(), center.getY(), radius, &regions, false);

	for (const auto& region : regions) {
		Vector3 polyStart;
		dtPolyRef startPoly;
		dtPolyRef ref;
		int status = 0;
		float pt[3];

		RecastNavMesh *mesh = region->getNavMesh();
		if (mesh == NULL)
			continue;

		dtNavMesh *dtNavMesh = mesh->getNavMesh();
		if (dtNavMesh == NULL)
			continue;

		ReadLocker rLocker(mesh->getLock());
		query->init(dtNavMesh, 2048);

		if (!((status = query->findNearestPoly(flipped.toFloatArray(), extents, &m_spawnFilter, &startPoly, polyStart.toFloatArray())) & DT_SUCCESS))
			continue;

		for (int i=0; i<50; i++) {
			try {
				if (!((status = query->findRandomPointAroundCircle(startPoly, polyStart.toFloatArray(), radius, &m_spawnFilter,
																   frand, &ref, pt)) & DT_SUCCESS)) {
					continue;
				} else {
					point = Vector3(pt[0], -pt[2], zone->getHeightNoCache(pt[0], -pt[2]));
					Vector3 temp = point - center;

					if ((temp.getX() * temp.getX() + temp.getY() * temp.getY()) > (radius * radius * 1.5f)) {
						info("Failed radius check: " + point.toString(), true);
						info("Center: " + flipped.toString(), true);
						info("Bad Poly: " + String::valueOf((uint64)ref), true);
						continue;
					}
					dtPolyRef path[64];
					dtRaycastHit hit;
					hit.path = path;
					hit.maxPath = 64;

					dtPolyRef dummy = 0;
					if (!((status = query->raycast(startPoly, polyStart.toFloatArray(), pt, &m_spawnFilter, 0, &hit, dummy)) & DT_SUCCESS)) {
						continue;
					}

					return true;
				}
			} catch (Exception& exc) {
				error(exc.getMessage());
			}
		}
	}

	return false;
}

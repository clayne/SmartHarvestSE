#include "PrecompiledHeaders.h"
#include "IRangeChecker.h"

AbsoluteRange::AbsoluteRange(const RE::TESObjectREFR* source, const double radius, const double verticalFactor) :
	m_sourceX(source->GetPositionX()), m_sourceY(source->GetPositionY()), m_sourceZ(source->GetPositionZ()),
	m_radius(radius), m_zLimit(radius * verticalFactor)
{
}

bool AbsoluteRange::IsValid(const RE::TESObjectREFR* refr, const double distance) const
{
	RE::FormID formID(refr->formID);
	double dx = fabs(refr->GetPositionX() - m_sourceX);
	double dy = fabs(refr->GetPositionY() - m_sourceY);
	double dz = fabs(refr->GetPositionZ() - m_sourceZ);

	// don't do Floating Point math if we can trivially see it's too far away
	if (dx > m_radius || dy > m_radius || dz > m_zLimit)
	{
		// very verbose
		DBG_DMESSAGE("REFR 0x%08x {%.2f,%.2f,%.2f} trivially too far from player {%.2f,%.2f,%.2f}",
			formID, refr->GetPositionX(), refr->GetPositionY(), refr->GetPositionZ(),
			m_sourceX, m_sourceY, m_sourceZ);
		m_distance = std::max({ dx, dy, dz });
		return false;
	}
	m_distance = distance > 0. ? distance : sqrt((dx * dx) + (dy * dy) + (dz * dz));
	DBG_VMESSAGE("REFR 0x%08x is %.2f units away, loot range %.2f XY, %.2f Z units", formID, m_distance, m_radius, m_zLimit);
	return m_distance <= m_radius;
}

double AbsoluteRange::Distance() const
{ 
	return m_distance;
}

BracketedRange::BracketedRange(const RE::TESObjectREFR* source, const double radius, const double delta) :
	m_innerLimit(source, radius, 1.0), m_outerLimit(source, radius + delta, 1.0)
{
}

// Don't calculate the distance twice - use value from first check as input to second
bool BracketedRange::IsValid(const RE::TESObjectREFR* refr, const double distance) const
{
	return !m_innerLimit.IsValid(refr, 0.) && m_outerLimit.IsValid(refr, m_innerLimit.Distance());
}

double BracketedRange::Distance() const
{
	return m_innerLimit.Distance();
}


/*
===========================================================================
Copyright (C) 2026, OpenJK-Experiment contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// g_ragdoll_math.h -- small vector, quaternion and rigid transform types used by the ragdoll code.
//
// The engine's own vec3_t is a bare float array, which makes the physics code hard to read.
// These types add the usual operators. Everything is inline and header only.
//
// Conventions:
//   * Quaternions are unit quaternions (w, x, y, z) describing a rotation.
//   * Rotation vectors are axis * angle in radians.
//   * A Transform maps a point from its local space into its parent space:
//       parentPoint = rotation * localPoint + position

#ifndef G_RAGDOLL_MATH_H
#define G_RAGDOLL_MATH_H

#include <math.h>

namespace ragdoll
{

const float PI_RADIANS = 3.14159265358979f;
const float SMALL_NUMBER = 1.0e-6f;

inline float DegreesToRadians( float degrees )	{ return degrees * ( PI_RADIANS / 180.0f ); }
inline float RadiansToDegrees( float radians )	{ return radians * ( 180.0f / PI_RADIANS ); }

inline float Clamp( float value, float minimum, float maximum )
{
	return value < minimum ? minimum : ( value > maximum ? maximum : value );
}

/*
==================
Vector3
==================
*/
struct Vector3
{
	float x, y, z;

	Vector3() : x( 0.0f ), y( 0.0f ), z( 0.0f ) {}
	Vector3( float xValue, float yValue, float zValue ) : x( xValue ), y( yValue ), z( zValue ) {}

	static Vector3 FromArray( const float *values )	{ return Vector3( values[0], values[1], values[2] ); }
	void CopyToArray( float *values ) const			{ values[0] = x; values[1] = y; values[2] = z; }

	Vector3 operator+( const Vector3 &other ) const	{ return Vector3( x + other.x, y + other.y, z + other.z ); }
	Vector3 operator-( const Vector3 &other ) const	{ return Vector3( x - other.x, y - other.y, z - other.z ); }
	Vector3 operator*( float scale ) const			{ return Vector3( x * scale, y * scale, z * scale ); }
	Vector3 operator/( float divisor ) const		{ return Vector3( x / divisor, y / divisor, z / divisor ); }
	Vector3 operator-() const						{ return Vector3( -x, -y, -z ); }
	Vector3 &operator+=( const Vector3 &other )		{ x += other.x; y += other.y; z += other.z; return *this; }
	Vector3 &operator-=( const Vector3 &other )		{ x -= other.x; y -= other.y; z -= other.z; return *this; }
	Vector3 &operator*=( float scale )				{ x *= scale; y *= scale; z *= scale; return *this; }
};

inline Vector3 operator*( float scale, const Vector3 &vector )	{ return vector * scale; }

inline float Dot( const Vector3 &a, const Vector3 &b )		{ return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float LengthSquared( const Vector3 &vector )			{ return Dot( vector, vector ); }
inline float Length( const Vector3 &vector )				{ return sqrtf( LengthSquared( vector ) ); }

inline Vector3 Cross( const Vector3 &a, const Vector3 &b )
{
	return Vector3( a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x );
}

// Returns the vector scaled to length one, or the zero vector if it is too short to normalize.
inline Vector3 Normalized( const Vector3 &vector )
{
	const float length = Length( vector );
	return ( length > SMALL_NUMBER ) ? vector / length : Vector3();
}

// Returns any unit vector perpendicular to the given unit vector.
inline Vector3 AnyPerpendicular( const Vector3 &unitVector )
{
	const Vector3 helper = ( fabsf( unitVector.x ) < 0.6f ) ? Vector3( 1.0f, 0.0f, 0.0f ) : Vector3( 0.0f, 1.0f, 0.0f );
	return Normalized( Cross( unitVector, helper ) );
}

// Returns the part of vector that is perpendicular to the unit vector normal.
inline Vector3 RemoveComponent( const Vector3 &vector, const Vector3 &unitNormal )
{
	return vector - unitNormal * Dot( vector, unitNormal );
}

/*
==================
Quaternion
==================
*/
struct Quaternion
{
	float w, x, y, z;

	Quaternion() : w( 1.0f ), x( 0.0f ), y( 0.0f ), z( 0.0f ) {}
	Quaternion( float wValue, float xValue, float yValue, float zValue ) : w( wValue ), x( xValue ), y( yValue ), z( zValue ) {}

	Vector3 VectorPart() const { return Vector3( x, y, z ); }
};

inline Quaternion operator*( const Quaternion &a, const Quaternion &b )
{
	return Quaternion(
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w );
}

inline Quaternion Conjugate( const Quaternion &rotation )
{
	return Quaternion( rotation.w, -rotation.x, -rotation.y, -rotation.z );
}

inline Quaternion NormalizedQuaternion( const Quaternion &rotation )
{
	const float length = sqrtf( rotation.w * rotation.w + rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z );
	if ( length < SMALL_NUMBER )
	{
		return Quaternion();
	}
	const float inverseLength = 1.0f / length;
	return Quaternion( rotation.w * inverseLength, rotation.x * inverseLength, rotation.y * inverseLength, rotation.z * inverseLength );
}

inline Vector3 Rotate( const Quaternion &rotation, const Vector3 &vector )
{
	// v' = v + 2w(q x v) + 2(q x (q x v)), the expanded form of q * v * q^-1
	const Vector3 axisPart = rotation.VectorPart();
	const Vector3 firstCross = Cross( axisPart, vector ) * 2.0f;
	return vector + firstCross * rotation.w + Cross( axisPart, firstCross );
}

inline Vector3 InverseRotate( const Quaternion &rotation, const Vector3 &vector )
{
	return Rotate( Conjugate( rotation ), vector );
}

inline Quaternion QuaternionFromAxisAngle( const Vector3 &unitAxis, float angleRadians )
{
	const float halfAngle = angleRadians * 0.5f;
	const float sineHalf = sinf( halfAngle );
	return Quaternion( cosf( halfAngle ), unitAxis.x * sineHalf, unitAxis.y * sineHalf, unitAxis.z * sineHalf );
}

inline Quaternion QuaternionFromRotationVector( const Vector3 &rotationVector )
{
	const float angle = Length( rotationVector );
	if ( angle < SMALL_NUMBER )
	{
		return Quaternion();
	}
	return QuaternionFromAxisAngle( rotationVector / angle, angle );
}

// Returns the rotation as axis * angle, choosing the shorter of the two equivalent rotations.
inline Vector3 RotationVectorFromQuaternion( const Quaternion &rotation )
{
	Quaternion shortest = rotation;
	if ( shortest.w < 0.0f )
	{
		shortest = Quaternion( -shortest.w, -shortest.x, -shortest.y, -shortest.z );
	}
	const Vector3 axisPart = shortest.VectorPart();
	const float sineHalf = Length( axisPart );
	if ( sineHalf < SMALL_NUMBER )
	{
		return axisPart * 2.0f;
	}
	const float angle = 2.0f * atan2f( sineHalf, shortest.w );
	return axisPart * ( angle / sineHalf );
}

// Returns the smallest rotation that turns unit vector from into unit vector to.
inline Quaternion ShortestArc( const Vector3 &from, const Vector3 &to )
{
	const float cosine = Clamp( Dot( from, to ), -1.0f, 1.0f );
	if ( cosine > 0.9999f )
	{
		return Quaternion();
	}
	if ( cosine < -0.9999f )
	{
		return QuaternionFromAxisAngle( AnyPerpendicular( from ), PI_RADIANS );
	}
	return QuaternionFromAxisAngle( Normalized( Cross( from, to ) ), acosf( cosine ) );
}

// Builds a quaternion from the columns of an orthonormal rotation matrix.
// rows[r][c] is the element in row r and column c.
inline Quaternion QuaternionFromRotationMatrix( const float rows[3][3] )
{
	const float trace = rows[0][0] + rows[1][1] + rows[2][2];
	Quaternion result;
	if ( trace > 0.0f )
	{
		const float scale = sqrtf( trace + 1.0f ) * 2.0f;
		result = Quaternion( 0.25f * scale,
							 ( rows[2][1] - rows[1][2] ) / scale,
							 ( rows[0][2] - rows[2][0] ) / scale,
							 ( rows[1][0] - rows[0][1] ) / scale );
	}
	else if ( rows[0][0] > rows[1][1] && rows[0][0] > rows[2][2] )
	{
		const float scale = sqrtf( 1.0f + rows[0][0] - rows[1][1] - rows[2][2] ) * 2.0f;
		result = Quaternion( ( rows[2][1] - rows[1][2] ) / scale,
							 0.25f * scale,
							 ( rows[0][1] + rows[1][0] ) / scale,
							 ( rows[0][2] + rows[2][0] ) / scale );
	}
	else if ( rows[1][1] > rows[2][2] )
	{
		const float scale = sqrtf( 1.0f + rows[1][1] - rows[0][0] - rows[2][2] ) * 2.0f;
		result = Quaternion( ( rows[0][2] - rows[2][0] ) / scale,
							 ( rows[0][1] + rows[1][0] ) / scale,
							 0.25f * scale,
							 ( rows[1][2] + rows[2][1] ) / scale );
	}
	else
	{
		const float scale = sqrtf( 1.0f + rows[2][2] - rows[0][0] - rows[1][1] ) * 2.0f;
		result = Quaternion( ( rows[1][0] - rows[0][1] ) / scale,
							 ( rows[0][2] + rows[2][0] ) / scale,
							 ( rows[1][2] + rows[2][1] ) / scale,
							 0.25f * scale );
	}
	return NormalizedQuaternion( result );
}

inline void RotationMatrixFromQuaternion( const Quaternion &rotation, float rows[3][3] )
{
	const float xx = rotation.x * rotation.x, yy = rotation.y * rotation.y, zz = rotation.z * rotation.z;
	const float xy = rotation.x * rotation.y, xz = rotation.x * rotation.z, yz = rotation.y * rotation.z;
	const float wx = rotation.w * rotation.x, wy = rotation.w * rotation.y, wz = rotation.w * rotation.z;

	rows[0][0] = 1.0f - 2.0f * ( yy + zz );	rows[0][1] = 2.0f * ( xy - wz );		rows[0][2] = 2.0f * ( xz + wy );
	rows[1][0] = 2.0f * ( xy + wz );		rows[1][1] = 1.0f - 2.0f * ( xx + zz );	rows[1][2] = 2.0f * ( yz - wx );
	rows[2][0] = 2.0f * ( xz - wy );		rows[2][1] = 2.0f * ( yz + wx );		rows[2][2] = 1.0f - 2.0f * ( xx + yy );
}

// Spherical interpolation between two rotations, fraction 0 gives from and 1 gives to.
inline Quaternion Slerp( const Quaternion &from, const Quaternion &to, float fraction )
{
	Quaternion target = to;
	float cosine = from.w * to.w + from.x * to.x + from.y * to.y + from.z * to.z;
	if ( cosine < 0.0f )
	{
		cosine = -cosine;
		target = Quaternion( -to.w, -to.x, -to.y, -to.z );
	}

	float fromWeight = 1.0f - fraction;
	float toWeight = fraction;
	if ( cosine < 0.9995f )
	{
		const float angle = acosf( cosine );
		const float inverseSine = 1.0f / sinf( angle );
		fromWeight = sinf( ( 1.0f - fraction ) * angle ) * inverseSine;
		toWeight = sinf( fraction * angle ) * inverseSine;
	}

	return NormalizedQuaternion( Quaternion(
		from.w * fromWeight + target.w * toWeight,
		from.x * fromWeight + target.x * toWeight,
		from.y * fromWeight + target.y * toWeight,
		from.z * fromWeight + target.z * toWeight ) );
}

/*
==================
Matrix3

A symmetric 3x3 matrix, used for inertia tensors.
==================
*/
struct Matrix3
{
	float element[3][3];

	Matrix3()
	{
		for ( int row = 0; row < 3; row++ )
		{
			for ( int column = 0; column < 3; column++ )
			{
				element[row][column] = 0.0f;
			}
		}
	}

	Vector3 operator*( const Vector3 &vector ) const
	{
		return Vector3(
			element[0][0] * vector.x + element[0][1] * vector.y + element[0][2] * vector.z,
			element[1][0] * vector.x + element[1][1] * vector.y + element[1][2] * vector.z,
			element[2][0] * vector.x + element[2][1] * vector.y + element[2][2] * vector.z );
	}
};

/*
==================
Transform

A rotation followed by a translation (no scale).
==================
*/
struct Transform
{
	Quaternion rotation;
	Vector3 position;

	Transform() {}
	Transform( const Quaternion &rotationValue, const Vector3 &positionValue ) : rotation( rotationValue ), position( positionValue ) {}

	Vector3 TransformPoint( const Vector3 &localPoint ) const		{ return Rotate( rotation, localPoint ) + position; }
	Vector3 InverseTransformPoint( const Vector3 &point ) const		{ return InverseRotate( rotation, point - position ); }
};

// Returns the transform that applies inner first and then outer.
inline Transform Compose( const Transform &outer, const Transform &inner )
{
	return Transform( NormalizedQuaternion( outer.rotation * inner.rotation ), outer.TransformPoint( inner.position ) );
}

inline Transform Inverse( const Transform &transform )
{
	const Quaternion inverseRotation = Conjugate( transform.rotation );
	return Transform( inverseRotation, Rotate( inverseRotation, -transform.position ) );
}

} // namespace ragdoll

#endif // G_RAGDOLL_MATH_H

// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'builder_kernel.dart';
import 'nodes.dart';
import '../elements/entities.dart';
import '../elements/types.dart';
import '../inferrer/abstract_value_domain.dart';
import '../js_model/type_recipe.dart';
import '../io/source_information.dart';
import '../universe/use.dart' show TypeUse;
import '../world.dart';

/// Enum that defines how a member has access to the current type variables.
enum ClassTypeVariableAccess {
  /// The member has no access to type variables.
  none,

  /// Type variables are accessible as a property on `this`.
  property,

  /// Type variables are accessible as parameters in the current context.
  parameter,

  /// If the current context is a generative constructor, type variables are
  /// accessible as parameters, otherwise type variables are accessible as
  /// a property on `this`.
  ///
  /// This is used for instance fields whose initializers are executed in the
  /// constructors.
  // TODO(johnniwinther): Avoid the need for this by adding a field-setter
  // to the J-model.
  instanceField,
}

/// Functions to insert type checking, coercion, and instruction insertion
/// depending on the environment for dart code.
abstract class TypeBuilder {
  final KernelSsaGraphBuilder builder;

  TypeBuilder(this.builder);

  JClosedWorld get _closedWorld => builder.closedWorld;

  AbstractValueDomain get _abstractValueDomain =>
      _closedWorld.abstractValueDomain;

  /// Create a type mask for 'trusting' a DartType. Returns `null` if there is
  /// no approximating type mask (i.e. the type mask would be `dynamic`).
  AbstractValue trustTypeMask(DartType type) {
    if (type == null) return null;
    type = builder.localsHandler.substInContext(type);
    type = type.unaliased;
    if (type.isDynamic) return null;
    if (!type.isInterfaceType) return null;
    if (type == _closedWorld.commonElements.objectType) return null;
    // The type element is either a class or the void element.
    ClassEntity element = (type as InterfaceType).element;
    return _abstractValueDomain.createNullableSubtype(element);
  }

  /// Create an instruction to simply trust the provided type.
  HInstruction _trustType(HInstruction original, DartType type) {
    assert(type != null);
    AbstractValue mask = trustTypeMask(type);
    if (mask == null) return original;
    return new HTypeKnown.pinned(mask, original);
  }

  /// Produces code that checks the runtime type is actually the type specified
  /// by attempting a type conversion.
  HInstruction _checkType(HInstruction original, DartType type) {
    assert(type != null);
    type = builder.localsHandler.substInContext(type);
    HInstruction other =
        buildTypeConversion(original, type, HTypeConversion.TYPE_CHECK);
    // TODO(johnniwinther): This operation on `registry` may be inconsistent.
    // If it is needed then it seems likely that similar invocations of
    // `buildTypeConversion` in `SsaBuilder.visitAs` should also be followed by
    // a similar operation on `registry`; otherwise, this one might not be
    // needed.
    builder.registry?.registerTypeUse(new TypeUse.isCheck(type));
    if (other is HTypeConversion && other.isRedundant(builder.closedWorld)) {
      return original;
    }
    return other;
  }

  /// Produces code that checks the runtime type is actually the type specified
  /// by attempting a type conversion.
  HInstruction _checkBoolConverion(HInstruction original) {
    var checkInstruction =
        HBoolConversion(original, _abstractValueDomain.boolType);
    if (checkInstruction.isRedundant(_closedWorld)) {
      return original;
    }
    DartType boolType = _closedWorld.commonElements.boolType;
    builder.registry?.registerTypeUse(new TypeUse.isCheck(boolType));
    return checkInstruction;
  }

  HInstruction trustTypeOfParameter(HInstruction original, DartType type) {
    if (type == null) return original;
    HInstruction trusted = _trustType(original, type);
    if (trusted == original) return original;
    if (trusted is HTypeKnown && trusted.isRedundant(builder.closedWorld)) {
      return original;
    }
    builder.add(trusted);
    return trusted;
  }

  HInstruction potentiallyCheckOrTrustTypeOfParameter(
      HInstruction original, DartType type) {
    if (type == null) return original;
    HInstruction checkedOrTrusted = original;
    if (builder.options.parameterCheckPolicy.isTrusted) {
      checkedOrTrusted = _trustType(original, type);
    } else if (builder.options.parameterCheckPolicy.isEmitted) {
      checkedOrTrusted = _checkType(original, type);
    }
    if (checkedOrTrusted == original) return original;
    builder.add(checkedOrTrusted);
    return checkedOrTrusted;
  }

  /// Depending on the context and the mode, wrap the given type in an
  /// instruction that checks the type is what we expect or automatically
  /// trusts the written type.
  HInstruction potentiallyCheckOrTrustTypeOfAssignment(
      HInstruction original, DartType type) {
    if (type == null) return original;
    HInstruction checkedOrTrusted = original;
    if (builder.options.assignmentCheckPolicy.isTrusted) {
      checkedOrTrusted = _trustType(original, type);
    } else if (builder.options.assignmentCheckPolicy.isEmitted) {
      checkedOrTrusted = _checkType(original, type);
    }
    if (checkedOrTrusted == original) return original;
    builder.add(checkedOrTrusted);
    return checkedOrTrusted;
  }

  HInstruction potentiallyCheckOrTrustTypeOfCondition(HInstruction original) {
    DartType boolType = _closedWorld.commonElements.boolType;
    HInstruction checkedOrTrusted = original;
    if (builder.options.conditionCheckPolicy.isTrusted) {
      checkedOrTrusted = _trustType(original, boolType);
    } else if (builder.options.conditionCheckPolicy.isEmitted) {
      checkedOrTrusted = _checkBoolConverion(original);
    }
    if (checkedOrTrusted == original) return original;
    builder.add(checkedOrTrusted);
    return checkedOrTrusted;
  }

  ClassTypeVariableAccess computeTypeVariableAccess(MemberEntity member);

  /// Helper to create an instruction that gets the value of a type variable.
  HInstruction addTypeVariableReference(
      TypeVariableType type, MemberEntity member,
      {SourceInformation sourceInformation}) {
    Local typeVariableLocal =
        builder.localsHandler.getTypeVariableAsLocal(type);

    /// Read [typeVariable] as a property of on `this`.
    HInstruction readAsProperty() {
      return readTypeVariable(type, member,
          sourceInformation: sourceInformation);
    }

    /// Read [typeVariable] as a parameter.
    HInstruction readAsParameter() {
      return builder.localsHandler
          .readLocal(typeVariableLocal, sourceInformation: sourceInformation);
    }

    ClassTypeVariableAccess typeVariableAccess;
    if (type.element.typeDeclaration is ClassEntity) {
      typeVariableAccess = computeTypeVariableAccess(member);
    } else {
      typeVariableAccess = ClassTypeVariableAccess.parameter;
    }
    switch (typeVariableAccess) {
      case ClassTypeVariableAccess.parameter:
        return readAsParameter();
      case ClassTypeVariableAccess.instanceField:
        if (member != builder.targetElement) {
          // When [member] is a field, we can either be generating a checked
          // setter or inlining its initializer in a constructor. An initializer
          // is never built standalone, so in that case [target] is not the
          // [member] itself.
          return readAsParameter();
        }
        return readAsProperty();
      case ClassTypeVariableAccess.property:
        return readAsProperty();
      case ClassTypeVariableAccess.none:
        builder.reporter.internalError(
            type.element, 'Unexpected type variable in static context.');
    }
    builder.reporter.internalError(
        type.element, 'Unexpected type variable access: $typeVariableAccess.');
    return null;
  }

  /// Generate code to extract the type argument from the object.
  HInstruction readTypeVariable(TypeVariableType variable, MemberEntity member,
      {SourceInformation sourceInformation}) {
    assert(member.isInstanceMember);
    assert(variable.element.typeDeclaration is ClassEntity);
    HInstruction target =
        builder.localsHandler.readThis(sourceInformation: sourceInformation);
    HInstruction interceptor =
        new HInterceptor(target, _abstractValueDomain.nonNullType)
          ..sourceInformation = sourceInformation;
    builder.add(interceptor);
    builder.push(new HTypeInfoReadVariable.intercepted(
        variable, interceptor, target, _abstractValueDomain.dynamicType)
      ..sourceInformation = sourceInformation);
    return builder.pop();
  }

  HInstruction buildTypeArgumentRepresentations(
      DartType type, MemberEntity sourceElement,
      [SourceInformation sourceInformation]) {
    assert(!type.isTypeVariable);
    // Compute the representation of the type arguments, including access
    // to the runtime type information for type variables as instructions.
    assert(type.isInterfaceType);
    InterfaceType interface = type;
    List<HInstruction> inputs = <HInstruction>[];
    for (DartType argument in interface.typeArguments) {
      inputs.add(analyzeTypeArgument(argument, sourceElement,
          sourceInformation: sourceInformation));
    }
    HInstruction representation = new HTypeInfoExpression(
        TypeInfoExpressionKind.INSTANCE,
        _closedWorld.elementEnvironment.getThisType(interface.element),
        inputs,
        _abstractValueDomain.dynamicType)
      ..sourceInformation = sourceInformation;
    return representation;
  }

  HInstruction analyzeTypeArgument(
      DartType argument, MemberEntity sourceElement,
      {SourceInformation sourceInformation}) {
    argument = argument.unaliased;
    if (argument.treatAsDynamic) {
      // Represent [dynamic] as [null].
      return builder.graph.addConstantNull(_closedWorld);
    }

    if (argument.isTypeVariable) {
      return addTypeVariableReference(argument, sourceElement,
          sourceInformation: sourceInformation);
    }

    List<HInstruction> inputs = <HInstruction>[];
    argument.forEachTypeVariable((TypeVariableType variable) {
      // TODO(johnniwinther): Also make this conditional on whether we have
      // calculated we need that particular method signature.
      inputs.add(analyzeTypeArgument(variable, sourceElement));
    });
    HInstruction result = new HTypeInfoExpression(
        TypeInfoExpressionKind.COMPLETE,
        argument,
        inputs,
        _abstractValueDomain.dynamicType)
      ..sourceInformation = sourceInformation;
    builder.add(result);
    return result;
  }

  HInstruction analyzeTypeArgumentNewRti(
      DartType argument, MemberEntity sourceElement,
      {SourceInformation sourceInformation}) {
    argument = argument.unaliased;

    if (argument.containsFreeTypeVariables) {
      // TODO(sra): Locate type environment.
      HInstruction environment =
          builder.graph.addConstantString("env", _closedWorld);
      // TODO(sra): Determine environment structure from context.
      TypeEnvironmentStructure structure = null;
      HInstruction rti = HTypeEval(environment, structure,
          TypeExpressionRecipe(argument), _abstractValueDomain.dynamicType)
        ..sourceInformation = sourceInformation;
      builder.add(rti);
      return rti;
    }

    HInstruction rti = HLoadType(argument, _abstractValueDomain.dynamicType)
      ..sourceInformation = sourceInformation;
    builder.add(rti);
    return rti;
  }

  /// Build a [HTypeConversion] for converting [original] to type [type].
  ///
  /// Invariant: [type] must be valid in the context.
  /// See [LocalsHandler.substInContext].
  HInstruction buildTypeConversion(
      HInstruction original, DartType type, int kind,
      {SourceInformation sourceInformation}) {
    if (builder.options.experimentNewRti) {
      return buildAsCheck(original, type,
          isTypeError: kind == HTypeConversion.TYPE_CHECK,
          sourceInformation: sourceInformation);
    }

    if (type == null) return original;
    type = type.unaliased;
    if (type.isInterfaceType && !type.treatAsRaw) {
      InterfaceType interfaceType = type;
      AbstractValue subtype =
          _abstractValueDomain.createNullableSubtype(interfaceType.element);
      HInstruction representations = buildTypeArgumentRepresentations(
          type, builder.sourceElement, sourceInformation);
      builder.add(representations);
      return new HTypeConversion.withTypeRepresentation(
          type, kind, subtype, original, representations)
        ..sourceInformation = sourceInformation;
    } else if (type.isTypeVariable) {
      AbstractValue subtype = original.instructionType;
      HInstruction typeVariable =
          addTypeVariableReference(type, builder.sourceElement);
      return new HTypeConversion.withTypeRepresentation(
          type, kind, subtype, original, typeVariable)
        ..sourceInformation = sourceInformation;
    } else if (type.isFunctionType || type.isFutureOr) {
      HInstruction reifiedType =
          analyzeTypeArgument(type, builder.sourceElement);
      // TypeMasks don't encode function types or FutureOr types.
      AbstractValue refinedMask = original.instructionType;
      return new HTypeConversion.withTypeRepresentation(
          type, kind, refinedMask, original, reifiedType)
        ..sourceInformation = sourceInformation;
    } else {
      return original.convertType(_closedWorld, type, kind)
        ..sourceInformation = sourceInformation;
    }
  }

  /// Build a [HAsCheck] for converting [original] to type [type].
  ///
  /// Invariant: [type] must be valid in the context.
  /// See [LocalsHandler.substInContext].
  HInstruction buildAsCheck(HInstruction original, DartType type,
      {bool isTypeError, SourceInformation sourceInformation}) {
    if (type == null) return original;
    type = type.unaliased;

    if (type.isDynamic) return original;
    if (type.isVoid) return original;
    if (type == _closedWorld.commonElements.objectType) return original;

    HInstruction reifiedType =
        analyzeTypeArgumentNewRti(type, builder.sourceElement);
    if (type is InterfaceType) {
      // TODO(sra): Under NNDB opt-in, this will be NonNullable.
      AbstractValue subtype =
          _abstractValueDomain.createNullableSubtype(type.element);
      return HAsCheck(original, reifiedType, isTypeError, subtype)
        ..sourceInformation = sourceInformation;
    } else {
      // TypeMasks don't encode function types or FutureOr types or type
      // variable types.
      AbstractValue abstractValue = original.instructionType;
      return HAsCheck(original, reifiedType, isTypeError, abstractValue)
        ..sourceInformation = sourceInformation;
    }
  }
}

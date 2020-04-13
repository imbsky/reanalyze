/* TypeScript file generated from ReasonComponent.re by genType. */
/* eslint-disable import/first */


const $$toRE552311971: { [key: string]: any } = {"A": 0};

// tslint:disable-next-line:no-var-requires
const CreateBucklescriptBlock = require('bs-platform/lib/es6/block.js');

// tslint:disable-next-line:no-var-requires
const Curry = require('bs-platform/lib/es6/curry.js');

// tslint:disable-next-line:no-var-requires
const ReasonComponentBS = require('./ReasonComponent.bs');

// tslint:disable-next-line:no-var-requires
const ReasonReact = require('reason-react/src/ReasonReact.js');

import {Mouse_t as ReactEvent_Mouse_t} from 'reason-react/src/ReactEvent.gen';

import {coord as Records_coord} from './Records.gen';

import {list} from './ReasonPervasives.gen';

import {t as Types_t} from './Types.gen';

// tslint:disable-next-line:interface-over-type-literal
export type person<a> = {
  readonly name: string; 
  readonly surname: string; 
  readonly type: string; 
  readonly polymorphicPayload: a
};

// tslint:disable-next-line:interface-over-type-literal
export type t = 
    "A"
  | { tag: "B"; value: number }
  | { tag: "C"; value: string };

export const onClick: (param:ReactEvent_Mouse_t) => void = ReasonComponentBS.onClick;

// tslint:disable-next-line:interface-over-type-literal
export type Props = {
  readonly message?: string; 
  readonly person: person<unknown>; 
  readonly intList?: list<number>; 
  readonly children?: unknown
};

export const ReasonComponent: React.ComponentType<Props> = ReasonReact.wrapReasonForJs(
  ReasonComponentBS.component,
  (function _(jsProps: Props) {
     return Curry._4(ReasonComponentBS.make, jsProps.message, {name:jsProps.person.name, surname:jsProps.person.surname, type_:jsProps.person.type, polymorphicPayload:jsProps.person.polymorphicPayload}, jsProps.intList, jsProps.children);
  }));

export default ReasonComponent;

export const minus: (_1:{ readonly first?: number; readonly second: number }) => number = function (Arg1: any) {
  const result = Curry._2(ReasonComponentBS.minus, Arg1.first, Arg1.second);
  return result
};

export const useTypeDefinedInAnotherModule: (x:Types_t) => Types_t = ReasonComponentBS.useTypeDefinedInAnotherModule;

export const tToString: (t:t) => string = function (Arg1: any) {
  const result = ReasonComponentBS.tToString(typeof(Arg1) === 'object'
    ? Arg1.tag==="B"
      ? CreateBucklescriptBlock.__(0, [Arg1.value])
      : CreateBucklescriptBlock.__(1, [Arg1.value])
    : $$toRE552311971[Arg1]);
  return result
};

export const useRecordsCoord: (param:Records_coord) => number = ReasonComponentBS.useRecordsCoord;

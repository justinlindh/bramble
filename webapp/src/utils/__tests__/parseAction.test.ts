import { describe, it, expect } from 'vitest';
import { parseAction } from '../parseAction';

describe('parseAction', () => {
  it('extracts the text of a well-formed CTCP ACTION', () => {
    expect(parseAction('\x01ACTION waves\x01')).toEqual({
      isAction: true,
      actionText: 'waves',
    });
  });

  it('accepts an empty action body', () => {
    expect(parseAction('\x01ACTION \x01')).toEqual({
      isAction: true,
      actionText: '',
    });
  });

  it('keeps interior spaces and punctuation in the action text', () => {
    expect(parseAction('\x01ACTION looks around, twice\x01')).toEqual({
      isAction: true,
      actionText: 'looks around, twice',
    });
  });

  it('rejects plain text', () => {
    expect(parseAction('just a message')).toEqual({
      isAction: false,
      actionText: '',
    });
  });

  it('requires the trailing delimiter', () => {
    expect(parseAction('\x01ACTION waves')).toEqual({
      isAction: false,
      actionText: '',
    });
  });

  it('requires the space after ACTION', () => {
    expect(parseAction('\x01ACTION\x01')).toEqual({
      isAction: false,
      actionText: '',
    });
  });
});
